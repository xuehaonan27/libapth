# LIBAPTH Integration Guide: JVM on RDMA Disaggregated Memory

## 1. Overview

This document describes how to use LIBAPTH as the threading substrate for a
JVM running on a disaggregated memory architecture, where a compute node
executes JVM bytecode and memory nodes provide extra heap via RDMA.

### Target Architecture

```
┌─────────────────────────────────────┐     RDMA      ┌──────────────────┐
│          Compute Node               │◄══════════════►│  Memory Node(s)  │
│                                     │   ibverbs      │                  │
│  ┌───────────────────────────────┐  │   user-space   │  Remote heap     │
│  │            JVM                │  │                │  objects         │
│  │  ┌─────┐ ┌─────┐ ┌─────┐    │  │                └──────────────────┘
│  │  │ GC  │ │Mut. │ │Mut. │... │  │
│  │  │thrd │ │thrd │ │thrd │    │  │
│  │  └──┬──┘ └──┬──┘ └──┬──┘    │  │
│  │     └───────┼───────┘        │  │
│  │             │                │  │
│  │     ┌───────▼─────────┐      │  │
│  │     │  LIBAPTH (apth) │      │  │
│  │     │  replaces NPTL  │      │  │
│  │     └─────────────────┘      │  │
│  └───────────────────────────────┘  │
└─────────────────────────────────────┘
```

### Why LIBAPTH for This Use Case

When a JVM mutator thread accesses a remote object, it must issue an RDMA
read/write and wait for the completion (5–50 μs network latency). During this
wait:

- **With pthreads (1:1 model)**: the kernel thread either busy-waits
  (wasting the CPU core) or blocks via a kernel context switch (~5 μs
  overhead per switch). With hundreds of mutator threads, the kernel
  scheduler thrashes.

- **With LIBAPTH (M:N model)**: the apth yields in userspace (~20 ns
  assembly context switch), and the scheduler immediately dispatches
  another apth on the same core. The RDMA wait is overlapped with useful
  work from other threads, with negligible switching overhead.

The benefit scales with concurrency: 100 mutators on 4 cores benefit from
~100x less context-switch overhead compared to pthreads.

## 2. RDMA Is Not FD I/O

**Critical**: RDMA via `libibverbs` does NOT use file descriptors for the data
path. The standard RDMA completion model is:

```c
// Post an RDMA read (userspace, no syscall)
ibv_post_send(qp, &send_wr, &bad_wr);

// Poll for completion (userspace memory read, no syscall)
struct ibv_wc wc;
while (ibv_poll_cq(cq, 1, &wc) == 0)
    ; // busy-spin
```

`ibv_poll_cq()` reads completion entries from a memory-mapped completion queue
(CQ). It is a pure userspace memory load — no syscall, no file descriptor, no
`epoll`. This means **LIBAPTH's I/O hooks (read, write, accept, epoll) and
the global reactor are irrelevant for RDMA completions**.

What you need instead: a mechanism to yield an apth when RDMA is in flight and
resume it when the CQ completion arrives. This requires a new event type and a
new polling component.

## 3. RDMA Completion Integration Design

### 3.1 New Event Type

Add `APTH_EVENT_TYPE_RDMA` to the event system:

```c
// In struct_apth_event_st.h:
enum apth_event_type {
    APTH_EVENT_TYPE_FD,
    APTH_EVENT_TYPE_SELECT,
    APTH_EVENT_TYPE_SIGS,
    APTH_EVENT_TYPE_TIME,
    APTH_EVENT_TYPE_SYNC,
    APTH_EVENT_TYPE_TID,
    APTH_EVENT_TYPE_FUNC,
    APTH_EVENT_TYPE_RDMA,     // <-- new
};

struct apth_event_rdma_st {
    struct ibv_cq *cq;        // completion queue to poll
    uint64_t wr_id;           // work request ID to match
    struct ibv_wc *wc_out;    // where to store the completion entry
};
```

### 3.2 Dedicated RDMA Poller Thread (Recommended Approach)

A dedicated pthread busy-polls all RDMA completion queues and wakes
schedulers when completions arrive:

```
┌───────────────────────────────────┐
│      RDMA Poller Thread           │
│                                   │
│   loop:                           │
│     for each registered CQ:      │
│       n = ibv_poll_cq(cq, ...)   │  ← userspace memory read, ~10ns
│       for each completion:       │
│         mark event OCCURRED      │
│         wake owning scheduler    │
│                                   │
│     if no completions:           │
│       brief pause / sched_yield  │
└───────────────────────────────────┘
```

#### Why a dedicated thread and not inline scheduler polling:

1. **Latency**: The poller checks CQs continuously without waiting for the
   scheduler to finish dispatching a thread. With inline polling, a long-
   running compute phase delays RDMA completion detection.

2. **Isolation**: The poller doesn't compete with user threads for scheduler
   time. It runs on its own CPU core (or shares a core with the reactor).

3. **Batching**: The poller can aggregate multiple completions before waking
   schedulers, reducing wake_eventfd writes.

### 3.3 User-Facing API

```c
// Wait for a single RDMA completion. Posts the work request before
// calling this, e.g., via ibv_post_send().
//
// Returns 0 on success (wc filled), -1 on error.
int apth_rdma_wait(struct ibv_cq *cq, uint64_t wr_id, struct ibv_wc *wc);

// Wait for a batch of RDMA completions (all wr_ids).
// Yields once, resumes when all completions are collected.
int apth_rdma_wait_batch(struct ibv_cq *cq, uint64_t *wr_ids, int count,
                          struct ibv_wc *wcs);

// Register a CQ with the RDMA poller. Must be called before
// apth_rdma_wait() on that CQ.
int apth_rdma_register_cq(struct ibv_cq *cq);

// Unregister a CQ.
void apth_rdma_unregister_cq(struct ibv_cq *cq);
```

### 3.4 Implementation: apth_rdma_wait()

```c
int apth_rdma_wait(struct ibv_cq *cq, uint64_t wr_id, struct ibv_wc *wc)
{
    // ---- Fast path: completion already available ----
    // Very common for cached/prefetched remote objects or short RDMA RTTs.
    // ibv_poll_cq is a userspace memory read (~10ns), no syscall.
    struct ibv_wc local_wc;
    int n = ibv_poll_cq(cq, 1, &local_wc);
    if (n > 0 && local_wc.wr_id == wr_id) {
        *wc = local_wc;
        return 0;  // Zero-yield fast path
    }

    // ---- Slow path: register with poller, yield, wait ----
    apth_t self = CUR_APTH;

    struct apth_event_st ev;
    memset(&ev, 0, sizeof(ev));
    ev.ev_type = APTH_EVENT_TYPE_RDMA;
    ev.ev_status = APTH_EV_STATUS_PENDING;
    ev.ev_args.RDMA.cq = cq;
    ev.ev_args.RDMA.wr_id = wr_id;
    ev.ev_args.RDMA.wc_out = wc;

    apth_event_list_add(&self->event_list, &ev);
    rdma_poller_submit(&ev, self, CUR_SCHED);

    atomic_store_release(&self->state, APTH_STATE_WAITING);
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    list_remove(&ev.elem);
    return (ev.ev_status == APTH_EV_STATUS_OCCURRED) ? 0 : -1;
}
```

**Performance properties**:
- Fast path: 0 syscalls, ~10ns (just ibv_poll_cq)
- Slow path: 0 syscalls for the yield itself (assembly context switch, ~20ns),
  plus the poller thread's CQ polling latency (~1-10μs depending on load)
- Total slow-path overhead: ~20ns (yield) + ~20ns (resume) = ~40ns of LIBAPTH
  overhead, vs ~5000ns for a kernel context switch with pthreads

### 3.5 RDMA Poller Thread Implementation

```c
struct rdma_poller {
    pthread_t thread;
    _Atomic(bool) running;

    // Registered CQs
    struct {
        struct ibv_cq *cq;
        bool active;
    } cqs[RDMA_MAX_CQS];
    int cq_count;
    lll_internal_t cq_lock;

    // Pending wait requests (from apth_rdma_wait slow path)
    struct {
        apth_event_t ev;
        apth_t th;
        apth_sched_t sched;
    } waiters[RDMA_MAX_WAITERS];
    _Atomic(int) waiter_count;
    lll_internal_t waiter_lock;
};

void *rdma_poller_func(void *arg) {
    struct rdma_poller *p = (struct rdma_poller *)arg;
    struct ibv_wc wc_batch[64];

    while (atomic_load(&p->running)) {
        bool any_work = false;

        // Poll all registered CQs
        for (int i = 0; i < p->cq_count; i++) {
            if (!p->cqs[i].active) continue;
            int n = ibv_poll_cq(p->cqs[i].cq, 64, wc_batch);
            if (n <= 0) continue;
            any_work = true;

            // Match completions to waiters
            for (int j = 0; j < n; j++) {
                match_and_wake_waiter(p, &wc_batch[j]);
            }
        }

        if (!any_work) {
            // Brief pause to avoid burning CPU when idle
            for (int i = 0; i < 32; i++)
                __builtin_ia32_pause();
        }
    }
    return NULL;
}
```

### 3.6 CQ-to-Scheduler Affinity

For cache locality, bind CQs to schedulers. Each scheduler's mutator threads
share a CQ, and the poller polls CQs in NUMA-aware order:

```
Scheduler 0 (Core 0) ←→ CQ 0 (mutators 0-N)
Scheduler 1 (Core 1) ←→ CQ 1 (mutators N-2N)
Scheduler 2 (Core 2) ←→ CQ 2 (GC workers)
Scheduler 3 (Core 3) ←→ CQ 3 (mutators 2N-3N)
```

This keeps completion data in the scheduler's L1/L2 cache and avoids
cross-NUMA CQ traffic.

## 4. JVM Thread Classification

The JVM runs fundamentally different thread types that benefit from different
scheduling strategies:

| Thread Type | Count | Behavior | LIBAPTH Strategy |
|---|---|---|---|
| **Mutator threads** | Hundreds | Frequent remote accesses, mostly I/O-bound on RDMA | `apth`, APTH_CLASS_IO_BOUND, yield on RDMA wait |
| **GC workers** | 4-8 | CPU-bound during GC, coordinate with mutators via barriers | `apth`, APTH_CLASS_CPU_BOUND, preemption enabled, distribute evenly across schedulers |
| **GC coordinator** | 1 | Triggers STW pauses, mostly sleeping | `apth`, normal priority |
| **JIT compiler** | 1-2 | CPU-bound, long compilation phases | Raw `pthread` on dedicated core, or `apth` with APTH_CLASS_CPU_BOUND |
| **Finalizer** | 1 | Mostly sleeping, brief bursts | `apth`, normal priority |
| **Reference handler** | 1 | Mostly sleeping | `apth`, normal priority |
| **Signal dispatcher** | 1 | JVM internal signal handling | Raw `pthread` (must handle real signals) |

### Thread Class API

```c
enum apth_thread_class {
    APTH_CLASS_DEFAULT,       // Normal FIFO scheduling
    APTH_CLASS_IO_BOUND,      // Prioritize on wake (front of ready queue)
    APTH_CLASS_CPU_BOUND,     // Subject to preemption, back of ready queue
    APTH_CLASS_REALTIME,      // Pin to specific scheduler, minimal yield
};

// Set via attributes before thread creation:
apth_attr_setclass_np(&attr, APTH_CLASS_IO_BOUND);
```

When a mutator wakes from RDMA (an I/O-bound thread completing I/O), it
should be dispatched immediately — push to the FRONT of the ready queue,
not the back. This minimizes the time between RDMA completion and the
thread resuming execution.

## 5. Preemption for Concurrent GC

During concurrent GC phases (e.g., G1 concurrent marking), GC workers and
mutators run simultaneously. GC workers are CPU-bound (scanning/marking the
heap). Without preemption, a GC worker on a scheduler starves all mutators
on that scheduler.

**Required**: Compile LIBAPTH with `APTH_PREEMPT_SIGNAL` or
`APTH_PREEMPT_INSTRUMENT`.

For JIT-compiled code, `APTH_PREEMPT_INSTRUMENT` is ideal:
- The JIT compiler inserts `__cyg_profile_func_enter` calls at function
  entries (via `-finstrument-functions` or equivalent JIT instrumentation)
- LIBAPTH's hook checks a per-scheduler counter and yields after N function
  entries
- Deterministic, no signals, no timer interrupts

For interpreted code and GC workers (compiled without instrumentation),
`APTH_PREEMPT_SIGNAL` provides a fallback.

## 6. Safepoint Integration

JVM safepoints (stop-the-world pauses) require all mutator threads to reach
a "safe" state. With LIBAPTH:

1. **GC coordinator** sets a global safepoint flag
2. **Mutators** check this flag at JVM safepoint poll sites (loop backedges,
   method entries, allocation sites — already present in JIT-compiled code)
3. When a mutator sees the flag, it calls `apth_yield()` and enters a
   "safepoint parked" state
4. GC coordinator waits until all mutators are parked
5. GC runs, clears the flag, signals all mutators to resume

For mutators that are yielded due to RDMA wait: they are already in
APTH_STATE_WAITING. The GC coordinator can consider them "at a safepoint"
because their stack is frozen and scannable. No additional yield is needed.

This is a significant advantage over pthreads: with pthreads, a thread
blocked in a futex_wait (kernel space) must be brought back to userspace
to reach a safepoint. With LIBAPTH, a thread in APTH_STATE_WAITING is
already in userspace with a frozen, scannable stack.

## 7. JVM Integration Strategy

### 7.1 Approach A: LD_PRELOAD (Quick Prototyping)

Use LIBAPTH's `LD_PRELOAD` mechanism to intercept pthread calls:

```bash
LD_PRELOAD=/path/to/libapth.so java -Xmx4g MyApplication
```

**Pros**: No JVM source changes. Quick to test.

**Cons**:
- JVM's internal pthreads (signal handler, GC coordinator) are also
  intercepted, which may cause issues
- `Object.wait()`, `Thread.sleep()`, `LockSupport.park()` use `futex(2)`
  which LIBAPTH does not hook
- JNI native code may call libc directly, bypassing hooks
- No RDMA integration (only replaces threading, not object access)
- Fragile across JVM versions

**Use for**: initial proof-of-concept to validate that LIBAPTH can run
the JVM at all, and to measure baseline context-switch improvements.

### 7.2 Approach B: JVM Source Modification (Production)

Modify OpenJDK HotSpot's threading and memory subsystems:

#### Threading layer (`src/hotspot/os/linux/`)

| File | Change |
|---|---|
| `os_linux.cpp` | Replace `pthread_create` → `apth_create` for mutator/GC threads |
| `os_linux.cpp` | Replace `pthread_mutex_*` → `apth_mutex_*` for internal locks |
| `os_linux.cpp` | Keep `pthread_create` for JIT compiler, signal dispatcher |
| `os::PlatformEvent` | Replace `futex`-based park/unpark → `apth_cond_wait`/`apth_cond_signal` |
| `os::naked_yield` | Replace `sched_yield()` → `apth_yield()` |
| `Monitor/Mutex` | Replace `pthread_mutex` → `apth_mutex` |

#### Object access barrier (`src/hotspot/share/oops/`)

| File | Change |
|---|---|
| `oop.hpp` / `access.hpp` | Add remote-object check in `oop_load` / `oop_store` |
| `barrierSet.hpp` | New `RDMABarrierSet` that calls `apth_rdma_wait` for remote objects |

#### JIT compiler (`src/hotspot/share/opto/` or `src/hotspot/cpu/x86/`)

| File | Change |
|---|---|
| `c2_MacroAssembler_x86.cpp` | Emit RDMA barrier code at object access sites |
| `templateInterpreter_x86.cpp` | Insert RDMA barrier into interpreted getfield/putfield |

#### GC (`src/hotspot/share/gc/`)

| File | Change |
|---|---|
| `gcThread.cpp` | Create GC workers with `APTH_CLASS_CPU_BOUND` |
| `safepointMechanism.cpp` | Treat APTH_STATE_WAITING threads as "at safepoint" |

### 7.3 Approach C: Project Loom Integration (Java 21+)

If using a modern JVM with Project Loom (virtual threads), an alternative
approach:

1. Use Java virtual threads as the M:N scheduling mechanism (JVM-provided)
2. Integrate RDMA into the `Continuation.yield()` mechanism
3. When a virtual thread accesses a remote object, it `Continuation.yield()`s
   with an RDMA completion handle
4. The carrier thread picks up another virtual thread
5. When RDMA completes, the virtual thread is rescheduled

**Pros**: Leverages JVM's existing M:N scheduler. Less invasive than full
threading replacement. Supported by Oracle/OpenJDK.

**Cons**: Loom's scheduler is Java-level (G1-friendly but not as
lightweight as LIBAPTH's assembly context switch). Less control over
scheduling policy.

## 8. NUMA Considerations

On multi-socket compute nodes:

1. **Pin LIBAPTH schedulers to NUMA nodes**: Each scheduler's worker pthread
   should be `CPU_SET` to cores on a specific NUMA node.

2. **Pin RDMA QPs to local NIC**: If the compute node has multiple NICs
   (one per socket), create QPs on the NIC closest to the scheduler's
   NUMA node.

3. **Local heap allocation**: When a mutator allocates an object, prefer
   local NUMA memory. When an object is evicted to remote memory, track
   which memory node it's on.

4. **RDMA CQ placement**: Allocate CQ memory on the NUMA node of the
   polling thread (poller or scheduler).

LIBAPTH's `APTH_NUMA` flag (currently a stub) should be implemented to
create one reactor + one RDMA poller per NUMA node.

## 9. Performance Expectations

### RDMA latency budget

| Operation | Latency | Notes |
|---|---|---|
| Local object access | ~1 ns | L1 cache hit |
| RDMA post_send | ~100-500 ns | Userspace doorbell ring |
| RDMA network RTT | 2-50 μs | Depends on network (InfiniBand ~2μs, RoCE ~5-10μs) |
| ibv_poll_cq | ~10-50 ns | Userspace memory read |
| LIBAPTH apth_yield | ~20 ns | Assembly context switch |
| LIBAPTH scheduler dispatch | ~50-200 ns | Queue pop + context switch |
| Kernel context switch | ~3-10 μs | For comparison with pthreads |

### Expected throughput scaling

| Mutator threads | Cores | pthread (kernel schedule) | LIBAPTH (userspace) |
|---|---|---|---|
| 4 | 4 | Baseline (no contention) | ~Same (no scheduling advantage) |
| 16 | 4 | ~10% overhead from kernel scheduling | ~Same as baseline |
| 100 | 4 | ~30-50% overhead (kernel thrashing) | ~5% overhead (userspace scheduling) |
| 1000 | 4 | Severe thrashing, 8GB stack memory | <10% overhead, 128MB stacks |

The sweet spot: **many mutator threads (>>cores) with frequent RDMA waits**.
If most accesses are local (<1% remote), the RDMA yield overhead is
negligible and LIBAPTH's advantage comes mainly from reduced thread creation
cost and smaller stacks.

## 10. Implementation Roadmap

### Phase 1: RDMA Event Type and Poller
- Add `APTH_EVENT_TYPE_RDMA` to event system
- Implement RDMA poller thread
- Implement `apth_rdma_wait()` API
- Test with standalone RDMA microbenchmark (no JVM)

### Phase 2: Thread Classification
- Add `apth_thread_class` to `apth_attr_t`
- Implement priority-aware ready queue (IO_BOUND threads to front)
- Test with mixed workload (CPU-bound + I/O-bound apths)

### Phase 3: JVM Threading Layer
- Modify HotSpot to use LIBAPTH for mutator/GC threads
- Replace futex-based park/unpark with apth_cond
- Validate with standard Java benchmarks (DaCapo, SPECjbb)

### Phase 4: Object Access Barrier
- Implement RDMA barrier in interpreter and JIT compiler
- Integrate `apth_rdma_wait()` into barrier slow path
- Test with disaggregated memory simulator

### Phase 5: NUMA and Production
- Implement `APTH_NUMA` for multi-socket nodes
- CQ-to-scheduler affinity
- Production benchmarks and tuning

## 11. Build Configuration for JVM Use

```makefile
# Recommended LIBAPTH configuration for JVM:
CFLAGS := -Wall -Wextra -std=gnu11 -g -O2 -fPIC \
    -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
    -DAPTH_CUR_USING_KEYWORD \
    -DAPTH_HOLD_INITIALIZER_PTHREAD \
    -DAPTH_PREEMPT_SIGNAL \
    -DAPTH_PREEMPT_QUANTUM_MS=5 \
    -DAPTH_USE_IOURING

# For JIT-compiled code with instrumentation preemption:
# Replace -DAPTH_PREEMPT_SIGNAL with:
#   -DAPTH_PREEMPT_INSTRUMENT
#   -DAPTH_PREEMPT_INSTRUMENT_THRESHOLD=5000
# And compile JIT output with -finstrument-functions
```

## 12. References

- LIBAPTH source: this repository
- libibverbs API: `man ibv_post_send`, `man ibv_poll_cq`
- OpenJDK HotSpot threading: `src/hotspot/os/linux/os_linux.cpp`
- Project Loom: JEP 444 (Virtual Threads)
- InfiniBand RDMA latency: typically 2-5 μs for 4KB reads
- LIBAPTH assembly context switch: `src/internal/apth_ctx_x86_64.S` (~20ns)
