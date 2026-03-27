# JVM HotSpot Integration Guide for LIBAPTH

This document specifies exactly how to modify OpenJDK 21 HotSpot to use
LIBAPTH as its threading substrate and to add RDMA-based disaggregated
memory support. It is written for a developer (or LLM coding assistant)
who will implement the HotSpot changes by following this guide step by step.

Target: OpenJDK 21 LTS (`jdk21u` repository).
Platform: x86-64 Linux only.

---

## Quick Reference: LIBAPTH Public APIs for JVM

Every API listed below is declared in `<apth.h>`. This table covers only the
functions and constants relevant to JVM integration; the full POSIX-like
threading API (create, join, mutex, cond, etc.) is omitted for brevity.

### Initialization and Lifecycle

| API | Signature | Description |
|-----|-----------|-------------|
| `apth_init_library` | `int apth_init_library(int workers)` | Library-mode init: starts scheduler workers in background. Caller's pthread continues normally. Returns 0 on success. |
| `apth_drop` | `void apth_drop(void)` | Tear down all LIBAPTH state: stops reactor, RDMA poller, scheduler workers. |
| `apth_create` | `int apth_create(apth_t *newthr, const apth_attr_t *attr, void *(*start)(void *), void *arg)` | Create a new thread. Class/stack/detach set via attr. |
| `apth_join` | `int apth_join(apth_t tid, void **value)` | Wait for thread termination, retrieve return value. |
| `apth_exit` | `void apth_exit(void *retval)` | Terminate calling thread. |
| `apth_yield` | `int apth_yield(void)` | Yield to scheduler (~20ns for M:N, kernel sched_yield for dedicated). |
| `apth_self` | `apth_t apth_self(void)` | Return handle of calling thread. |
| `apth_get_worker_count` | `int apth_get_worker_count(void)` | Return number of scheduler worker pthreads. |

### Thread Attributes

| API | Signature | Description |
|-----|-----------|-------------|
| `apth_attr_init` | `int apth_attr_init(apth_attr_t *attr)` | Initialize attribute object with defaults. |
| `apth_attr_destroy` | `int apth_attr_destroy(apth_attr_t *attr)` | Destroy attribute object. |
| `apth_attr_setclass_np` | `int apth_attr_setclass_np(apth_attr_t *attr, int class)` | Set thread scheduling class. |
| `apth_attr_getclass_np` | `int apth_attr_getclass_np(const apth_attr_t *attr, int *class)` | Get thread scheduling class. |
| `apth_attr_setstacksize` | `int apth_attr_setstacksize(apth_attr_t *attr, size_t size)` | Set thread stack size (min 16KB). |
| `apth_attr_setdetachstate` | `int apth_attr_setdetachstate(apth_attr_t *attr, int state)` | Set joinable or detached. |
| `apth_attr_setname_np` | `int apth_attr_setname_np(apth_attr_t *attr, const char *name)` | Set thread name (debugging). |
| `apth_attr_setsigmask_np` | `int apth_attr_setsigmask_np(apth_attr_t *attr, const sigset_t *mask)` | Set initial signal mask. |
| `apth_attr_setaffinity_np` | `int apth_attr_setaffinity_np(apth_attr_t *attr, size_t cpusetsize, const cpu_set_t *cpuset)` | Set CPU affinity (dedicated threads only). |

### Thread Classes

| Constant | Value | Behavior |
|----------|-------|----------|
| `APTH_CLASS_DEFAULT` | 0 | Normal FIFO scheduling |
| `APTH_CLASS_IO_BOUND` | 1 | Priority dispatch (front of ready queue on wake) |
| `APTH_CLASS_CPU_BOUND` | 2 | Back of ready queue, subject to preemption |
| `APTH_CLASS_REALTIME` | 3 | Pin to specific scheduler, minimal yield |
| `APTH_CLASS_DEDICATED` | 4 | Own 1:1 pthread, bypasses M:N scheduler entirely |
| `APTH_CLASS_DISTRIBUTED` | 5 | Round-robin across schedulers on creation |

### Thread Inspection

| API | Signature | Description |
|-----|-----------|-------------|
| `apth_getstate` | `int apth_getstate(apth_t th)` | Returns `APTH_THREAD_STATE_*` constant or -1. |
| `apth_get_saved_sp` | `int apth_get_saved_sp(apth_t th, void **sp_out)` | Get saved stack pointer of yielded thread. EBUSY if RUNNING. ENOTSUP for dedicated. |
| `apth_get_stack_bounds` | `int apth_get_stack_bounds(apth_t th, void **base_out, size_t *size_out)` | Get usable stack memory range (after guard page). ENOTSUP for dedicated. |
| `apth_get_thread_stats` | `int apth_get_thread_stats(apth_t th, struct apth_thread_stats *stats)` | Get dispatches, cpu_time, wall_time, class, state. |
| `apth_for_each_thread` | `int apth_for_each_thread(apth_thread_visitor_t visitor, void *arg)` | Iterate all live threads (all schedulers + dedicated). Returns count. |

### Thread State Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `APTH_THREAD_STATE_NEW` | 0x02 | Created but not yet dispatched |
| `APTH_THREAD_STATE_READY` | 0x04 | On ready queue, waiting for CPU |
| `APTH_THREAD_STATE_WAITING` | 0x08 | Yielded, waiting for event (I/O, RDMA, cond, etc.) |
| `APTH_THREAD_STATE_TERMINATED` | 0x10 | Finished execution |
| `APTH_THREAD_STATE_WAKED` | 0x20 | Event occurred, pending re-dispatch |
| `APTH_THREAD_STATE_RUNNING` | 0x40 | Currently executing on a worker |

### Safepoint Cooperation

| API | Signature | Description |
|-----|-----------|-------------|
| `apth_request_pause_all` | `int apth_request_pause_all(void)` | Stop all schedulers from dispatching. Blocks until no thread is RUNNING. |
| `apth_resume_all` | `int apth_resume_all(void)` | Resume all schedulers after pause. |
| `apth_set_state_callback` | `int apth_set_state_callback(apth_state_callback_t cb, void *arg)` | Register callback fired on every thread state transition. |
| `apth_set_preempt_hook` | `int apth_set_preempt_hook(apth_preempt_hook_t hook, void *arg)` | Register callback fired before preemption yield. |

### Synchronization Primitives

| Type | Init | Lock/Wait | Unlock/Signal | Notes |
|------|------|-----------|---------------|-------|
| `apth_mutex_t` | `apth_mutex_init` | `apth_mutex_lock`, `apth_mutex_timedlock`, `apth_mutex_trylock` | `apth_mutex_unlock` | Supports NORMAL, ERRORCHECK, RECURSIVE. Works for both M:N and dedicated threads. |
| `apth_cond_t` | `apth_cond_init` | `apth_cond_wait`, `apth_cond_timedwait` | `apth_cond_signal`, `apth_cond_broadcast` | Supports CLOCK_MONOTONIC and CLOCK_REALTIME. Works for both M:N and dedicated threads. |
| `apth_barrier_t` | `apth_barrier_init` | `apth_barrier_wait` | (auto) | Mixed M:N + dedicated safe. |
| `apth_sem_t` | `apth_sem_init` | `apth_sem_wait`, `apth_sem_timedwait`, `apth_sem_trywait` | `apth_sem_post` | Mixed safe. |
| `apth_rwlock_t` | `apth_rwlock_init` | `apth_rwlock_rdlock`, `apth_rwlock_wrlock`, timed/try variants | `apth_rwlock_unlock` | Mixed safe. |

### Signal Handling

| API | Signature | Description |
|-----|-----------|-------------|
| `apth_kill` | `int apth_kill(apth_t t, int sig)` | Send signal to specific thread. |
| `apth_sigmask` | `int apth_sigmask(int how, const sigset_t *set, sigset_t *oldset)` | Get/set calling thread's signal mask. |

### NUMA (requires `-DAPTH_NUMA`)

| API | Signature | Description |
|-----|-----------|-------------|
| `apth_get_numa_node_count` | `int apth_get_numa_node_count(void)` | Number of NUMA nodes (>=1). |
| `apth_get_thread_numa_node` | `int apth_get_thread_numa_node(apth_t th)` | NUMA node of thread's scheduler. -1 for dedicated. |

### RDMA (requires `-DAPTH_USE_RDMA`)

| API | Signature | Description |
|-----|-----------|-------------|
| `apth_rdma_register_cq` | `int apth_rdma_register_cq(struct ibv_cq *cq)` | Register CQ with RDMA poller. Must call before any wait on that CQ. |
| `apth_rdma_unregister_cq` | `void apth_rdma_unregister_cq(struct ibv_cq *cq)` | Remove CQ from poller. |
| `apth_rdma_wait` | `int apth_rdma_wait(struct ibv_cq *cq, uint64_t wr_id, struct ibv_wc *wc)` | Wait for one RDMA completion. Fast path: ~10ns. Slow path: yield + resume ~70ns. |
| `apth_rdma_wait_batch` | `int apth_rdma_wait_batch(struct ibv_cq *cq, uint64_t *wr_ids, int count, struct ibv_wc *wcs)` | Wait for multiple RDMA completions in one yield. |

---

## Build Configuration

### Step 1: Build LIBAPTH for JVM Use

```bash
cd /path/to/libapth

# Edit Makefile to set these CFLAGS:
CFLAGS := -Wall -Wextra -std=gnu11 -g -O2 -fPIC \
    -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
    -DAPTH_CUR_USING_KEYWORD \
    -DAPTH_HOLD_INITIALIZER_PTHREAD \
    -DAPTH_PREEMPT_SIGNAL \
    -DAPTH_PREEMPT_QUANTUM_MS=5 \
    -DAPTH_NUMA \
    -DAPTH_USE_IOURING \
    -DAPTH_USE_RDMA

# Add to LDFLAGS:
LDFLAGS += -libverbs

make clean && make all
sudo make install
```

Flag rationale:
- `APTH_CUR_USING_KEYWORD`: Uses `_Thread_local` for scheduler TLS (faster than pthread TLS).
- `APTH_HOLD_INITIALIZER_PTHREAD`: Required for library-mode init pattern.
- `APTH_PREEMPT_SIGNAL`: Signal-based preemption using SIGPROF timer.
  Ensures CPU-bound GC workers and mutators in tight loops yield fairly.
- `APTH_PREEMPT_QUANTUM_MS=5`: 5ms preemption quantum (200Hz). Faster than
  the default 10ms, balancing GC pause sensitivity with preemption overhead.
- `APTH_NUMA`: Enables NUMA node detection and per-node scheduler binding.
- `APTH_USE_IOURING`: Uses io_uring for async I/O on supported kernels (5.1+).
- `APTH_USE_RDMA`: Enables RDMA completion poller.

### Step 2: Configure OpenJDK Build

```bash
cd /path/to/jdk21u

./configure \
    --with-extra-cflags="-I/path/to/libapth/src -DAPTH_USE_RDMA -DAPTH_NUMA" \
    --with-extra-cxxflags="-I/path/to/libapth/src -DAPTH_USE_RDMA -DAPTH_NUMA" \
    --with-extra-ldflags="-L/path/to/libapth/build/lib -lapth -libverbs -ldl"
```

Alternatively, edit `make/hotspot/lib/CompileJvm.gmk` directly:

```makefile
JVM_LIBS += -lapth -libverbs -ldl
JVM_CFLAGS += -I/path/to/libapth/src -DAPTH_USE_RDMA -DAPTH_NUMA
```

### Step 3: Build

```bash
make images CONF=linux-x86_64-server-release
```

---

## 1. Library-Mode Initialization from JVM

### Problem

The standard LIBAPTH entry point (`APTH_MAIN_BEGIN` / `apth_init`) takes
over the calling thread: it creates workers, spawns a "main apth", and
blocks the original pthread until the process exits. The JVM cannot use
this pattern because:

1. The JVM's `main()` thread must remain a regular pthread (it runs
   `JNI_CreateJavaVM`, handles signals, waits for shutdown).
2. LIBAPTH threads must be created later, on demand, from `os::create_thread()`.

### Solution: `apth_init_library()`

`apth_init_library(int workers)` initializes all LIBAPTH subsystems
(signal system, FD table, reactor, preemption, scheduler pool) without
creating a "main apth" or blocking the calling thread. The caller's
pthread continues as a normal thread and can create apth threads via
`apth_create()` at any point afterward.

Internally, `apth_init_library`:
1. Calls `apth_init_common(workers)` which initializes all subsystems
   and creates `workers` scheduler worker pthreads.
2. Sets up a dummy scheduler (id=-1) for the calling pthread so that
   `CUR_SCHED` TLS works if needed.
3. Uses a static sentinel object as `MAIN_APTH` to satisfy the workers'
   spin-wait (they check `MAIN_APTH != NULL` before entering their main
   loop).
4. Sets `__apth_library_mode = true` so workers do not self-terminate
   when the sentinel is seen as "exited".

### Where to Call

The JVM's `main()` calls `JNI_CreateJavaVM()` which calls
`Threads::create_vm()` which calls `os::init_2()`. This is where
LIBAPTH should be initialized:

```cpp
// File: src/hotspot/os/linux/os_linux.cpp

#include <apth.h>

jint os::init_2(void) {
    // ... existing initialization (page sizes, memory, etc.) ...

    // Initialize LIBAPTH in library mode.
    // Worker count = physical CPU cores. Do NOT count hyperthreads:
    // each worker is pinned to a core, and HT siblings are better used
    // for the RDMA poller and reactor pthreads.
    int apth_workers = os::active_processor_count();
    if (apth_workers > 1) {
        // Reserve 1-2 cores for JVM infrastructure pthreads
        // (signal dispatcher, watcher, JIT compiler)
        apth_workers -= 2;
        if (apth_workers < 1) apth_workers = 1;
    }

    int ret = apth_init_library(apth_workers);
    if (ret != 0) {
        vm_exit_during_initialization("Failed to initialize LIBAPTH");
        return JNI_ERR;
    }

    // The RDMA poller thread is started automatically the first time
    // apth_rdma_register_cq() is called — no manual start needed.
    // It is stopped automatically by apth_drop() during shutdown.

    // ... rest of os::init_2() ...
    return JNI_OK;
}
```

### Key Points

- **No `APTH_MAIN_BEGIN` needed.** The JVM main thread stays a pthread.
- **No `APTH_CONFIG` needed.** Worker count is passed directly to
  `apth_init_library()`.
- **CUR_SCHED is set** for the calling pthread (dummy scheduler id=-1),
  so LIBAPTH macros work from the main thread context. However, the main
  thread cannot yield (it has no scheduler), so it should not call
  `apth_yield()` or `apth_mutex_lock()` from the main pthread. Use
  `apth_create()` with `APTH_CLASS_DEDICATED` for any main-thread work
  that needs sync primitives.
- **Worker count selection**: physical CPU cores minus a small reserve
  for dedicated pthreads (JIT, signal handler, watcher). On a 32-core
  machine, 30 workers is a reasonable default.

---

## 2. Thread Class Selection for JVM Thread Types

### Overview

The JVM creates several types of threads, each with different scheduling
requirements. LIBAPTH's thread class system maps directly to these needs.
The key decision is: which threads should be M:N scheduled (fast userspace
context switch, I/O hooks active) and which should be 1:1 dedicated pthreads
(raw blocking I/O, no scheduler involvement)?

### Class Assignment Table

| JVM Thread Type | HotSpot `ThreadType` | LIBAPTH Class | Rationale |
|-----------------|---------------------|---------------|-----------|
| Java mutator threads | `os::java_thread` | `APTH_CLASS_IO_BOUND` | Primary beneficiaries of RDMA latency hiding. Priority dispatch (front of ready queue) when waking from RDMA completion ensures data-ready threads resume with minimal delay. |
| GC worker threads | `os::gc_thread` | `APTH_CLASS_DISTRIBUTED` | CPU-bound parallel work. `DISTRIBUTED` assigns workers round-robin across schedulers, ensuring even spread across cores. Avoids overloading a single scheduler with all GC workers. |
| VM operations thread | `os::vm_thread` | `APTH_CLASS_CPU_BOUND` | Runs VM operations (safepoints, deoptimization). CPU-bound, scheduled normally. Back of ready queue, subject to preemption. |
| JIT compiler threads | `os::compiler_thread` | `APTH_CLASS_DEDICATED` | CPU-bound long-running compilation. Needs its own pthread for uninterrupted execution. No benefit from M:N scheduling. I/O hooks are bypassed (dedicated threads use raw blocking syscalls). |
| Signal dispatcher | `os::os_thread` | `APTH_CLASS_DEDICATED` | Must be a real kernel thread to receive OS signals (`SIGINT`, `SIGTERM`, `SIGHUP`). Cannot be an M:N thread because signal delivery targets pthreads, not apths. |
| Watcher thread | `os::watcher_thread` | `APTH_CLASS_DEDICATED` | Periodic timer thread. Uses `timerfd` / `nanosleep` for periodic wake-ups. Dedicated pthread ensures it is not starved by other threads. |

### Code Example: `os::create_thread()`

```cpp
// File: src/hotspot/os/linux/os_linux.cpp

#include <apth.h>

// Map HotSpot thread type to LIBAPTH thread class.
// Returns -1 if the thread type is unknown (should not happen).
static int apth_class_for(os::ThreadType thr_type) {
    switch (thr_type) {
    case os::java_thread:     return APTH_CLASS_IO_BOUND;
    case os::gc_thread:       return APTH_CLASS_DISTRIBUTED;
    case os::vm_thread:       return APTH_CLASS_CPU_BOUND;
    case os::compiler_thread: return APTH_CLASS_DEDICATED;
    case os::os_thread:       return APTH_CLASS_DEDICATED;
    case os::watcher_thread:  return APTH_CLASS_DEDICATED;
    default:                  return APTH_CLASS_DEFAULT;
    }
}

bool os::create_thread(Thread* thread, ThreadType thr_type,
                       size_t req_stack_size) {
    OSThread* osthread = new OSThread();
    thread->set_osthread(osthread);

    // Determine stack size
    size_t stack_size = req_stack_size;
    if (stack_size == 0) {
        if (thr_type == os::java_thread)
            stack_size = JavaThread::stack_size_at_create();
        else
            stack_size = os::default_stack_size(thr_type);
    }

    // All JVM threads now go through apth_create.
    // DEDICATED threads get their own pthread automatically.
    // M:N threads are scheduled by LIBAPTH's userspace scheduler.
    int thread_class = apth_class_for(thr_type);

    apth_t tid;
    apth_attr_t attr;
    apth_attr_init(&attr);
    apth_attr_setstacksize(&attr, stack_size);
    apth_attr_setclass_np(&attr, thread_class);

    // Set a descriptive thread name for debugging
    const char *name = NULL;
    switch (thr_type) {
    case os::java_thread:     name = "JVM-Mutator"; break;
    case os::gc_thread:       name = "JVM-GC-Worker"; break;
    case os::vm_thread:       name = "JVM-VM-Thread"; break;
    case os::compiler_thread: name = "JVM-JIT"; break;
    case os::os_thread:       name = "JVM-Signal"; break;
    case os::watcher_thread:  name = "JVM-Watcher"; break;
    default:                  name = "JVM-Unknown"; break;
    }
    if (name) apth_attr_setname_np(&attr, name);

    int ret = apth_create(&tid, &attr, thread_native_entry, thread);
    apth_attr_destroy(&attr);

    if (ret != 0) {
        delete osthread;
        return false;
    }

    osthread->set_apth_id(tid);
    // Note: is_dedicated is stored in the apth_t handle internally.
    // OSThread tracks the apth_id uniformly for both M:N and dedicated.

    return true;
}
```

### OSThread Modifications

```cpp
// File: src/hotspot/os/linux/os_linux.hpp

class OSThread : public CHeapObj<mtThread> {
    pthread_t _pthread_id;   // Keep for backward compatibility
    apth_t    _apth_id;      // LIBAPTH thread handle (all thread types)
    // ...
public:
    void set_apth_id(apth_t id) { _apth_id = id; }
    apth_t apth_id() const { return _apth_id; }

    // Convenience: check if this is an M:N scheduled thread
    // (i.e., not a DEDICATED pthread).
    // Uses apth_getstate which works for all thread types.
    bool is_apth_mn() const {
        // Dedicated threads have is_dedicated=true internally.
        // We can check the thread class via apth_get_thread_stats.
        struct apth_thread_stats stats;
        if (apth_get_thread_stats(_apth_id, &stats) == 0)
            return stats.thread_class != APTH_CLASS_DEDICATED;
        return false;
    }
};
```

### Why DISTRIBUTED for GC Workers

The GC creates N worker threads (typically 4-8) for parallel phases. If all
are assigned to the same scheduler (which happens with `APTH_CLASS_CPU_BOUND`
since creation defaults to the current scheduler), they compete for a single
CPU core. `APTH_CLASS_DISTRIBUTED` distributes them round-robin across all
schedulers, so each GC worker gets its own core for parallel mark/compact.

---

## 3. Safepoint Implementation

### Background

HotSpot uses safepoints to stop all Java threads for GC, deoptimization,
and other VM operations. `SafepointSynchronize::begin()` sets a flag and
waits until all Java threads have reached a safe state.

With LIBAPTH, many mutator threads may be in `WAITING` state (yielded for
RDMA, I/O, or sync primitives). These threads have frozen, scannable stacks
and are inherently at a safepoint. This can significantly reduce safepoint
pause time.

### API: `apth_request_pause_all()` / `apth_resume_all()`

`apth_request_pause_all()`:
1. Sets an atomic `__apth_global_pause` flag.
2. Wakes all schedulers (via eventfd) so they see the flag promptly.
3. Spin-waits until no scheduler has a `RUNNING` thread (i.e., `cur == NULL`
   for all schedulers).
4. Returns 0 when all schedulers are paused.

After this call:
- No M:N thread is executing. All are in READY, WAITING, WAKED, or NEW state.
- Dedicated threads are NOT paused (they are kernel threads, not controlled
  by the LIBAPTH scheduler). The JVM must handle dedicated threads separately
  (they use existing safepoint polling).
- All M:N thread stacks are frozen and safe to walk.

`apth_resume_all()`:
1. Clears the `__apth_global_pause` flag.
2. Wakes all schedulers so they resume dispatching.

### API: `apth_getstate()`

```c
int state = apth_getstate(th);
```

Returns one of `APTH_THREAD_STATE_*` constants. For safepoint purposes:

- `APTH_THREAD_STATE_WAITING` (0x08): Thread is yielded, waiting for an
  event. Stack is frozen. **Already at safepoint.**
- `APTH_THREAD_STATE_RUNNING` (0x40): Thread is executing on a worker.
  Must reach a safepoint poll to stop.
- `APTH_THREAD_STATE_READY` (0x04) / `APTH_THREAD_STATE_WAKED` (0x20):
  Thread is not executing but is ready to be dispatched. Stack is frozen
  if it was previously saved. **Safe after `apth_request_pause_all()`**
  (scheduler will not dispatch it).

### API: `apth_set_state_callback()`

```c
void my_state_callback(apth_t th, int old_state, int new_state, void *arg);
apth_set_state_callback(my_state_callback, jvm_context);
```

Called synchronously from the scheduler every time a thread transitions
between states. Useful for tracking which threads have reached a safe state
without polling. The callback runs in scheduler context (not in the
transitioning thread's context), so it must be fast and lock-free.

### Integration with `SafepointSynchronize`

```cpp
// File: src/hotspot/share/runtime/safepoint.cpp

#include <apth.h>

// Safepoint request context
struct JVMSafepointContext {
    volatile int threads_at_safepoint;
    int total_java_threads;
};

static JVMSafepointContext _sp_ctx;

// State callback: count threads that enter WAITING/READY state
static void safepoint_state_cb(apth_t th, int old_state, int new_state,
                                void *arg) {
    JVMSafepointContext *ctx = (JVMSafepointContext *)arg;
    if (new_state == APTH_THREAD_STATE_WAITING ||
        new_state == APTH_THREAD_STATE_READY) {
        __atomic_fetch_add(&ctx->threads_at_safepoint, 1, __ATOMIC_RELAXED);
    }
}

void SafepointSynchronize::begin() {
    // 1. Set the safepoint flag so running threads poll and yield
    _state = _synchronizing;
    set_safepoint_id(++_safepoint_counter);

    // 2. Register state callback for immediate notification
    _sp_ctx.threads_at_safepoint = 0;
    _sp_ctx.total_java_threads = count_java_threads();
    apth_set_state_callback(safepoint_state_cb, &_sp_ctx);

    // 3. Pause all LIBAPTH schedulers.
    // This blocks until no M:N thread is RUNNING.
    // After return, all M:N threads are guaranteed stopped.
    apth_request_pause_all();

    // 4. Handle dedicated threads (JIT, signal, watcher) separately.
    // These are real pthreads and use the existing safepoint polling
    // mechanism (safepoint poll page).
    wait_for_dedicated_threads_to_reach_safepoint();

    // 5. All threads are now at safepoint
    _state = _synchronized;
    // ... proceed with GC / deopt / etc. ...
}

void SafepointSynchronize::end() {
    // 1. Clear the safepoint flag
    _state = _not_synchronized;

    // 2. Unregister the callback
    apth_set_state_callback(NULL, NULL);

    // 3. Resume all LIBAPTH schedulers
    apth_resume_all();

    // 4. Resume dedicated threads (existing mechanism)
    resume_dedicated_threads();
}
```

### Key Points

- `apth_request_pause_all()` is the primary mechanism. It guarantees no
  M:N thread is running when it returns.
- Threads in `WAITING` state were already effectively at a safepoint
  before the pause was requested. The pause just prevents new dispatches.
- The state callback is optional but useful for monitoring how quickly
  threads reach safepoint.
- Dedicated threads (JIT, signal, watcher) are NOT affected by
  `apth_request_pause_all()`. They must be handled by the existing
  safepoint polling mechanism.

---

## 4. GC Stack Walking

### Background

During GC, HotSpot must scan every Java thread's stack for object references
(roots). For running threads, the stack is walked from the thread's current
frame. For yielded LIBAPTH threads, the stack pointer is saved in the TCB's
assembly context and must be retrieved via the public API.

### API: `apth_get_saved_sp()`

```c
int apth_get_saved_sp(apth_t th, void **sp_out);
```

Returns the saved RSP register value from the thread's context structure.
This is the stack pointer at the point where the thread yielded (called
`apth_ctx_swap`). The saved context contains callee-saved registers
(RBX, RBP, R12-R15) plus the return address pushed by `CALL`, so the
frame at `*sp_out` is the yield point.

Return values:
- `0`: Success, `*sp_out` contains the saved stack pointer.
- `ESRCH`: Invalid thread handle.
- `EBUSY`: Thread is `RUNNING` (stack pointer is not saved, it is live
  on the worker pthread's CPU).
- `ENOTSUP`: Thread is `DEDICATED` (uses pthread stack, not an apth stack).

### API: `apth_get_stack_bounds()`

```c
int apth_get_stack_bounds(apth_t th, void **base_out, size_t *size_out);
```

Returns the usable stack memory region:
- `*base_out`: Start of usable stack memory (after guard page). On x86-64
  with downward-growing stacks, this is the lowest address that the thread
  may access.
- `*size_out`: Usable stack size in bytes (total allocation minus guard page).

The stack walker must walk from `saved_sp` upward (toward higher addresses)
toward `base_out + size_out` (the stack top / initial RSP).

### API: `apth_for_each_thread()`

```c
typedef int (*apth_thread_visitor_t)(apth_t th, void *arg);
int apth_for_each_thread(apth_thread_visitor_t visitor, void *arg);
```

Iterates all live threads across all scheduler queues (new, ready, waiting,
waked, running, terminated) plus all dedicated threads. The visitor is called
for each thread. Return 0 from the visitor to continue, non-zero to stop
early. Returns the total count of threads visited.

The iteration holds scheduler queue locks briefly per queue, so it should
be called during a safepoint (when no threads are dispatched) for consistency.

### Code Example: `JavaThread::oops_do()`

```cpp
// File: src/hotspot/share/runtime/thread.cpp

#include <apth.h>

void JavaThread::oops_do(OopClosure* f, CodeBlobClosure* cf) {
    apth_t apth_id = this->osthread()->apth_id();
    int state = apth_getstate(apth_id);

    if (state == APTH_THREAD_STATE_RUNNING) {
        // Thread is currently executing (should not happen during safepoint
        // for M:N threads after apth_request_pause_all, but possible for
        // dedicated threads).
        // Use the existing stack walking mechanism for running threads.
        oops_do_running(f, cf);
        return;
    }

    // --- M:N thread stack walking ---

    // Get the saved stack pointer (RSP at yield point)
    void *saved_sp = NULL;
    int ret = apth_get_saved_sp(apth_id, &saved_sp);

    if (ret == ENOTSUP) {
        // Dedicated thread: use existing pthread-based stack walking
        oops_do_running(f, cf);
        return;
    }

    if (ret != 0 || saved_sp == NULL) {
        // Error: thread invalid or still running (should not happen)
        return;
    }

    // Get stack bounds for safety checks
    void *stack_base = NULL;
    size_t stack_size = 0;
    apth_get_stack_bounds(apth_id, &stack_base, &stack_size);

    // Reconstruct the topmost frame from the saved SP.
    //
    // At the yield point, the assembly context switch (apth_ctx_swap) has:
    //   1. Pushed callee-saved registers (RBP, RBX, R12-R15)
    //   2. Saved RSP into the context
    //
    // The return address at the top of the saved stack points back to
    // the C code that called apth_ctx_swap (the scheduler yield path).
    // We need to unwind past the LIBAPTH frames to reach the user's
    // (JVM's) frames.
    //
    // The frame chain is:
    //   saved_sp -> apth_ctx_swap return
    //            -> apth_yield return
    //            -> user code (JVM frame)

    // Walk from saved SP to stack top
    uintptr_t sp = (uintptr_t)saved_sp;
    uintptr_t stack_top = (uintptr_t)stack_base + stack_size;

    // Skip LIBAPTH internal frames (apth_ctx_swap, apth_yield, scheduler).
    // These frames have no Java references.
    // The first JVM frame is identified by checking if the PC is in JVM code.
    frame fr = frame((intptr_t*)sp);

    // Unwind past LIBAPTH frames
    while (fr.sp() < (intptr_t*)stack_top) {
        address pc = fr.pc();
        if (CodeCache::contains(pc) || Interpreter::contains(pc)) {
            // Found a JVM frame
            break;
        }
        fr = fr.sender_for_raw_compiled_frame();
    }

    // Now walk JVM frames and scan for oops
    while (fr.sp() < (intptr_t*)stack_top) {
        fr.oops_do(f, cf, &_regmap);
        if (fr.is_entry_frame()) break;
        fr = fr.sender(&_regmap);
    }
}
```

### GC Root Scanning Loop

```cpp
// File: src/hotspot/share/gc/shared/gcRootScan.cpp

#include <apth.h>

struct GCRootScanCtx {
    OopClosure *closure;
    int scanned_count;
};

// Visitor callback for apth_for_each_thread
static int gc_scan_visitor(apth_t th, void *arg) {
    GCRootScanCtx *ctx = (GCRootScanCtx *)arg;

    // Get the JavaThread associated with this apth
    // (The JVM stores a mapping from apth_t -> JavaThread* in a table
    //  or as thread-local data accessible via apth_getspecific)
    JavaThread *jthread = lookup_java_thread(th);
    if (jthread == NULL)
        return 0;  // Not a Java thread (GC worker, VM thread, etc.)

    // Scan this thread's stack
    jthread->oops_do(ctx->closure, NULL);
    ctx->scanned_count++;

    return 0;  // Continue iteration
}

void gc_scan_all_thread_roots(OopClosure *f) {
    GCRootScanCtx ctx;
    ctx.closure = f;
    ctx.scanned_count = 0;

    // Must be called during safepoint (after apth_request_pause_all)
    int total = apth_for_each_thread(gc_scan_visitor, &ctx);

    log_debug(gc)("GC root scan: visited %d threads, scanned %d Java threads",
                   total, ctx.scanned_count);
}
```

### Key Points

- Always call `apth_request_pause_all()` before stack walking. This
  guarantees no M:N thread is `RUNNING`, so `apth_get_saved_sp()` will
  not return `EBUSY`.
- Dedicated threads (`ENOTSUP` from `apth_get_saved_sp`) must use the
  existing pthread-based stack walking.
- The LIBAPTH assembly context saves only callee-saved registers (RBP,
  RBX, R12-R15) plus the return address. The stack walker must unwind
  past 2-3 LIBAPTH internal frames to reach JVM frames.
- Stack bounds from `apth_get_stack_bounds()` should be used for safety:
  never walk past `base + size`.

---

## 5. RDMA Barrier Set Integration

### Background

In disaggregated memory, some Java objects reside on remote memory nodes.
When a JVM mutator thread accesses a field of a remote object, the access
must be transparently redirected through an RDMA fetch. LIBAPTH's
`apth_rdma_wait()` is the key function that hides RDMA latency: it yields
the calling thread in ~20ns, allowing other mutators to run on the same
core while the RDMA operation completes.

### How `apth_rdma_wait()` Works

```
Thread A:                     RDMA Poller:              Scheduler:
  ibv_post_send(qp, wr)
  apth_rdma_wait(cq, wr_id, &wc)
    |-- fast path: ibv_poll_cq(cq)
    |   [if completion found: return immediately, ~10ns]
    |
    |-- slow path:
    |   register waiter with poller
    |   set state = WAITING
    |   apth_yield() [~20ns]
    |                                                    dispatch Thread B
    |                           ibv_poll_cq(cq) [~10ns]
    |                           match wr_id -> Thread A
    |                           mark event OCCURRED
    |                           wake scheduler [~20ns]
    |                                                    dispatch Thread A
    |   resume, return wc [~20ns]
```

- **Fast path** (~10ns): `ibv_poll_cq` is a userspace memory-mapped read.
  If the completion is already available (short RDMA RTT or prefetched
  data), the function returns immediately with zero context switches.
- **Slow path** (~70ns total): yield (~20ns) + poller detect (~10ns) +
  wake (~20ns) + resume (~20ns). Compare with kernel context switch
  (~5000ns) for pthread-based blocking.

### `RDMABarrierSet::fetch_remote_object()` Pattern

```cpp
// File: src/hotspot/share/gc/rdma/rdmaBarrierSet.cpp

#include <apth.h>
#include <infiniband/verbs.h>

// Address range for remote objects (Option B from parent guide)
#define REMOTE_HEAP_START 0x600000000000ULL
#define REMOTE_HEAP_END   0x700000000000ULL

static inline bool is_remote(oop obj) {
    uintptr_t addr = (uintptr_t)(void*)obj;
    return addr >= REMOTE_HEAP_START && addr < REMOTE_HEAP_END;
}

// Per-thread RDMA resources (stored in JavaThread or thread-local)
struct ThreadRDMAContext {
    struct ibv_qp *qp;      // Queue pair for this thread's connection
    struct ibv_cq *cq;      // Completion queue (may be shared or per-thread)
    struct ibv_mr *local_mr; // Memory region for local buffers
    uint64_t next_wr_id;    // Monotonically increasing WR ID
};

oop RDMABarrierSet::fetch_remote_object(oop remote_ref) {
    ThreadRDMAContext *rdma_ctx = get_rdma_context_for_current_thread();

    // 1. Determine remote address and object size
    uintptr_t remote_addr = get_remote_addr(remote_ref);
    size_t obj_size = get_object_size(remote_ref);

    // 2. Allocate local buffer for the fetched object
    void *local_buf = allocate_local_copy(obj_size);

    // 3. Build scatter-gather element
    struct ibv_sge sge;
    sge.addr = (uint64_t)(uintptr_t)local_buf;
    sge.length = obj_size;
    sge.lkey = rdma_ctx->local_mr->lkey;

    // 4. Build RDMA read work request
    uint64_t wr_id = rdma_ctx->next_wr_id++;
    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;
    wr.opcode = IBV_WR_RDMA_READ;
    wr.send_flags = IBV_SEND_SIGNALED;  // Generate completion
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = get_remote_rkey(remote_ref);

    // 5. Post the RDMA read
    struct ibv_send_wr *bad_wr;
    int ret = ibv_post_send(rdma_ctx->qp, &wr, &bad_wr);
    if (ret != 0) {
        fatal("ibv_post_send failed: %s", strerror(ret));
    }

    // 6. Wait for RDMA completion using LIBAPTH
    //    This is the critical call: it yields the mutator thread (~20ns)
    //    and lets other mutators run on the same core while the RDMA
    //    read traverses the network (~5-50us).
    struct ibv_wc wc;
    ret = apth_rdma_wait(rdma_ctx->cq, wr_id, &wc);
    if (ret != 0 || wc.status != IBV_WC_SUCCESS) {
        fatal("RDMA read failed: status=%d", wc.status);
    }

    // 7. Return the local copy as a local oop
    return cast_to_oop(local_buf);
}
```

### CQ Registration

Before any `apth_rdma_wait()` call, the CQ must be registered with the
RDMA poller:

```cpp
// During JVM initialization or per-thread RDMA setup:
struct ibv_cq *cq = ibv_create_cq(ctx, cq_depth, NULL, NULL, 0);
apth_rdma_register_cq(cq);  // Register with LIBAPTH's RDMA poller

// During teardown:
apth_rdma_unregister_cq(cq);
ibv_destroy_cq(cq);
```

### Batch RDMA Waits

For prefetching multiple objects (e.g., scanning an array of remote
references):

```cpp
// Post multiple RDMA reads, then wait for all at once
uint64_t wr_ids[batch_size];
struct ibv_wc wcs[batch_size];

for (int i = 0; i < batch_size; i++) {
    wr_ids[i] = post_rdma_read(remote_refs[i], local_bufs[i]);
}

// Single yield, resumes when ALL completions arrive
apth_rdma_wait_batch(cq, wr_ids, batch_size, wcs);
```

This is more efficient than individual waits because it yields only once
instead of `batch_size` times.

---

## 6. Signal Handling

### SIGSEGV: NullPointerException

HotSpot uses SIGSEGV for implicit null checks: accessing address 0 triggers
SIGSEGV, and the JVM's signal handler converts it to a NullPointerException.

LIBAPTH installs its own signal catchers at the process level but delivers
signals to individual threads via the per-thread `sigmask` field. The JVM's
SIGSEGV handler will be invoked on the correct thread because:

1. LIBAPTH's signal system dispatches signals to the thread that caused the
   fault (the kernel delivers SIGSEGV to the faulting thread, not a random one).
2. The JVM's `sigaction(SIGSEGV, ...)` handler is installed after LIBAPTH
   init, so it takes precedence.

**Recommendation**: Install the JVM's SIGSEGV handler after `apth_init_library()`
to ensure it overrides any LIBAPTH-internal handler. LIBAPTH does not claim
SIGSEGV for its own use.

### SIGPROF: Preemption vs. JVM Profiling

When LIBAPTH is compiled with `APTH_PREEMPT_SIGNAL`, it uses SIGPROF for
preemption. Each scheduler worker's SIGPROF timer fires every
`APTH_PREEMPT_QUANTUM_MS` milliseconds. This conflicts with JVM CPU
profiling that also uses SIGPROF.

**Solutions** (pick one):

1. **Use `apth_set_preempt_hook()`** (recommended): Register a JVM-aware
   preemption hook that also collects profiling data:

   ```cpp
   void jvm_preempt_hook(apth_t th, void *arg) {
       // Collect profiling sample (replaces SIGPROF-based JVM profiling)
       JavaThread *jthread = lookup_java_thread(th);
       if (jthread) {
           collect_cpu_sample(jthread);
       }
       // The preemption yield will happen automatically after this hook returns
   }

   // During init:
   apth_set_preempt_hook(jvm_preempt_hook, NULL);
   ```

2. **Use a different signal for JVM profiling**: Change JVM's AsyncGetCallTrace
   to use SIGVTALRM or a real-time signal (SIGRTMIN+N) instead of SIGPROF.

3. **Disable JVM CPU profiling**: If only RDMA/GC metrics are needed, JVM
   profiling via SIGPROF is unnecessary.

### Signal Mask Inheritance

LIBAPTH threads inherit the signal mask from their creator (the thread that
calls `apth_create()`). Alternatively, use `apth_attr_setsigmask_np()` to
set an explicit signal mask for the new thread:

```cpp
// Block all signals except those the JVM needs
sigset_t mask;
sigfillset(&mask);
sigdelset(&mask, SIGSEGV);    // JVM needs for null checks
sigdelset(&mask, SIGBUS);     // JVM needs for some memory errors
apth_attr_setsigmask_np(&attr, &mask);
```

### Sending Signals to Specific Threads

```c
apth_kill(target_thread, SIGQUIT);  // Thread dump
apth_kill(target_thread, SIGINT);   // Interrupt
```

For dedicated threads, `apth_kill()` forwards to `pthread_kill()` on the
backing pthread. For M:N threads, it sets the signal pending in the per-thread
signal mask and the thread processes it when next dispatched.

### Recommendation for JVM

Use `APTH_PREEMPT_SIGNAL` mode with `APTH_PREEMPT_QUANTUM_MS=5`. This
provides:
- Fair scheduling for CPU-bound GC workers and tight-loop mutators.
- 5ms quantum ensures responsive safepoint arrival.
- Dedicated threads (JIT, signal, watcher) automatically block SIGPROF
  (LIBAPTH does this in the dedicated thread wrapper).

---

## 7. Mixed Lock Patterns

### Background

The JVM uses many internal locks. Some are contended only by mutator/GC
threads (M:N scheduled), some only by dedicated threads (JIT, signal),
and some by both. Understanding the lock contention pattern is critical
for choosing the correct mutex implementation.

### DEDICATED Thread Interop

LIBAPTH's `APTH_CLASS_DEDICATED` threads can share all synchronization
primitives (`apth_mutex_t`, `apth_cond_t`, `apth_barrier_t`, `apth_sem_t`,
`apth_rwlock_t`) with regular M:N threads. The mechanism differs per
thread type:

| Operation | M:N Thread | Dedicated Thread |
|-----------|-----------|-----------------|
| `apth_mutex_lock` (contended) | Yields to scheduler (~20ns) | Blocks on eventfd (kernel sleep) |
| `apth_mutex_unlock` (waiter exists) | Wakes scheduler | Writes to eventfd |
| `apth_cond_wait` | Yields to scheduler | Blocks on eventfd |
| `apth_cond_signal` | Wakes scheduler / writes eventfd | Same |

This means **all JVM locks can uniformly use `apth_mutex_t`**, regardless
of whether they are contended by M:N threads, dedicated threads, or both.
The old guidance about needing `pthread_mutex_t` for mixed locks is
obsolete now that dedicated threads are supported.

### Lock Classification

Despite uniform `apth_mutex_t`, classifying locks helps optimize:

| Lock Category | Examples | Recommendation |
|--------------|----------|----------------|
| **Mutator-only** | Java monitor locks, safepoint locks | `apth_mutex_t`. High contention benefits from fast userspace yield. |
| **GC-only** | GC work stealing locks, marking bitmap locks | `apth_mutex_t`. GC workers are M:N (DISTRIBUTED), fast yield. |
| **VM-wide** | Threads_lock, Heap_lock, CodeCache_lock | `apth_mutex_t`. Contended by both M:N and dedicated threads. Works correctly with dedicated interop. |
| **JIT-only** | Compile queue lock, method data lock | `apth_mutex_t`. JIT threads are DEDICATED, will use eventfd path. |

### PlatformEvent Replacement

HotSpot's `PlatformEvent` (used by `Object.wait()`, `Thread.sleep()`,
`LockSupport.park()`) is the most critical synchronization structure.
Replace with `apth_cond_t` / `apth_mutex_t`:

```cpp
// File: src/hotspot/os/posix/os_posix.cpp

#include <apth.h>

class PlatformEvent : public CHeapObj<mtSynchronizer> {
    volatile int _event;
    apth_mutex_t _mutex;
    apth_cond_t  _cond;

public:
    PlatformEvent() : _event(0) {
        apth_mutex_init(&_mutex, NULL);

        // Use CLOCK_MONOTONIC for timed waits (matches JVM expectation)
        apth_condattr_t cattr;
        apth_condattr_init(&cattr);
        apth_condattr_setclock(&cattr, CLOCK_MONOTONIC);
        apth_cond_init(&_cond, &cattr);
        apth_condattr_destroy(&cattr);
    }

    ~PlatformEvent() {
        apth_cond_destroy(&_cond);
        apth_mutex_destroy(&_mutex);
    }

    // Park: block until unparked or timeout.
    // For M:N threads: yields to scheduler (~20ns) while waiting.
    // For dedicated threads: blocks on eventfd (kernel sleep).
    int park(jlong millis) {
        apth_mutex_lock(&_mutex);
        if (_event > 0) {
            _event = 0;
            apth_mutex_unlock(&_mutex);
            return OS_OK;
        }

        if (millis <= 0) {
            // Untimed wait
            while (_event <= 0) {
                apth_cond_wait(&_cond, &_mutex);
            }
        } else {
            // Timed wait
            struct timespec abstime;
            clock_gettime(CLOCK_MONOTONIC, &abstime);
            abstime.tv_sec += millis / 1000;
            abstime.tv_nsec += (millis % 1000) * 1000000;
            if (abstime.tv_nsec >= 1000000000L) {
                abstime.tv_sec++;
                abstime.tv_nsec -= 1000000000L;
            }
            while (_event <= 0) {
                int ret = apth_cond_timedwait(&_cond, &_mutex, &abstime);
                if (ret == ETIMEDOUT) break;
            }
        }

        int was_set = _event;
        _event = 0;
        apth_mutex_unlock(&_mutex);
        return (was_set > 0) ? OS_OK : OS_TIMEOUT;
    }

    // Unpark: wake a parked thread.
    // Safe to call from any thread type (M:N, dedicated, or even
    // the main pthread).
    void unpark() {
        apth_mutex_lock(&_mutex);
        _event = 1;
        apth_cond_signal(&_cond);
        apth_mutex_unlock(&_mutex);
    }

    // Reset the event (used by some JVM paths)
    void reset() {
        _event = 0;
    }
};
```

### PlatformMutex / PlatformMonitor Replacement

```cpp
// File: src/hotspot/os/posix/os_posix.cpp

class PlatformMutex : public CHeapObj<mtSynchronizer> {
    apth_mutex_t _mutex;
public:
    PlatformMutex() { apth_mutex_init(&_mutex, NULL); }
    ~PlatformMutex() { apth_mutex_destroy(&_mutex); }
    void lock()   { apth_mutex_lock(&_mutex); }
    void unlock() { apth_mutex_unlock(&_mutex); }
    bool try_lock() { return apth_mutex_trylock(&_mutex) == 0; }
};

class PlatformMonitor : public PlatformMutex {
    apth_cond_t _cond;
public:
    PlatformMonitor() {
        apth_condattr_t attr;
        apth_condattr_init(&attr);
        apth_condattr_setclock(&attr, CLOCK_MONOTONIC);
        apth_cond_init(&_cond, &attr);
        apth_condattr_destroy(&attr);
    }
    ~PlatformMonitor() { apth_cond_destroy(&_cond); }
    void wait() { apth_cond_wait(&_cond, &mutex_addr()); }
    int wait(jlong millis) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += millis / 1000;
        ts.tv_nsec += (millis % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        return apth_cond_timedwait(&_cond, &mutex_addr(), &ts);
    }
    void notify()     { apth_cond_signal(&_cond); }
    void notify_all() { apth_cond_broadcast(&_cond); }
};
```

---

## 8. Performance Tuning

### Worker Count

```
Recommended: workers = physical_CPU_cores - 2
```

- Subtract 2 for JVM infrastructure pthreads that are DEDICATED (JIT
  compiler, signal dispatcher). These run on their own pthreads and
  should not compete with scheduler workers for CPU time.
- On a 32-core machine: 30 workers.
- On a 4-core machine: 2 workers (minimum useful).
- Do NOT count hyperthreads. Each scheduler worker is pinned to a physical
  core. HT siblings are used by the RDMA poller and reactor.

### Stack Sizes

| Thread Type | Recommended Stack Size | Rationale |
|-------------|----------------------|-----------|
| Java mutator | 128KB-256KB | Default JVM is 1MB. With 1000 mutators at 1MB = 1GB of stacks. LIBAPTH provides guard page protection, so smaller stacks are safe. Stack overflow triggers SIGSEGV caught by JVM handler. |
| GC worker | 256KB | GC recursion depth is moderate. |
| VM thread | 512KB | May do deep recursive operations. |
| JIT compiler (dedicated) | 2MB | JIT compilation uses deep recursion for graph traversal. Keep at default or larger. Dedicated threads use pthread stacks, not LIBAPTH-managed stacks. |

```cpp
// In os::create_thread():
apth_attr_setstacksize(&attr, stack_size);
// Minimum is 16KB (LIBAPTH enforced). Guard pages are created automatically.
```

### Preemption Mode

Use `APTH_PREEMPT_SIGNAL` with 5ms quantum:
```
-DAPTH_PREEMPT_SIGNAL -DAPTH_PREEMPT_QUANTUM_MS=5
```

- 5ms quantum (200Hz) balances fairness with overhead.
- Ensures CPU-bound mutators (tight loops without I/O) yield for safepoint
  checks and GC workers.
- Each scheduler worker gets its own SIGPROF timer.
- Dedicated threads automatically block SIGPROF (handled by LIBAPTH).

### RDMA Polling Configuration

```cpp
// Register each CQ before use:
apth_rdma_register_cq(cq);

// Limits (compile-time):
// APTH_RDMA_MAX_CQS     = 64   (max registered CQs)
// APTH_RDMA_MAX_WAITERS  = 4096 (max concurrent RDMA waits)
```

The RDMA poller uses adaptive backoff:
1. Hot spin: 64 iterations of `PAUSE` instruction (~40 cycles each).
2. Warm spin: `sched_yield()` up to 1024 iterations.
3. Cold: `nanosleep(100us)` when no completions are found.

For high-throughput RDMA workloads, the poller stays in hot spin mode
(~10ns per CQ poll).

### LIBAPTH Compile Flags (Complete)

```makefile
CFLAGS := -Wall -Wextra -std=gnu11 -g -O2 -fPIC \
    -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
    -DAPTH_CUR_USING_KEYWORD \
    -DAPTH_HOLD_INITIALIZER_PTHREAD \
    -DAPTH_PREEMPT_SIGNAL \
    -DAPTH_PREEMPT_QUANTUM_MS=5 \
    -DAPTH_NUMA \
    -DAPTH_USE_IOURING \
    -DAPTH_USE_RDMA
```

| Flag | Purpose |
|------|---------|
| `APTH_CUR_USING_KEYWORD` | C11 `_Thread_local` for fast TLS access. |
| `APTH_HOLD_INITIALIZER_PTHREAD` | Required for correct shutdown behavior. |
| `APTH_PREEMPT_SIGNAL` | SIGPROF-based preemption for fairness. |
| `APTH_PREEMPT_QUANTUM_MS=5` | 5ms quantum for responsive safepoints. |
| `APTH_NUMA` | NUMA topology detection, per-node scheduler binding. |
| `APTH_USE_IOURING` | io_uring for async file I/O on supported kernels. |
| `APTH_USE_RDMA` | RDMA completion poller for disaggregated memory. |

### `apth_get_thread_stats()` for Monitoring

```c
struct apth_thread_stats {
    int dispatches;          // How many times the thread was dispatched
    double cpu_time_sec;     // Cumulative CPU time
    double wall_time_sec;    // Wall time since creation
    int thread_class;        // APTH_CLASS_*
    int state;               // Current APTH_THREAD_STATE_*
};

struct apth_thread_stats stats;
apth_get_thread_stats(th, &stats);
```

Key metrics per thread:
- **dispatches**: Number of times scheduled. High dispatch rate (>10K/s)
  suggests the thread yields frequently (good for I/O-bound mutators).
- **cpu_time_sec**: Actual compute time. Compare with `wall_time_sec` to
  compute CPU utilization per thread.
- **state**: Current state. In steady state, most mutators should alternate
  between RUNNING and WAITING (RDMA wait).

---

## 9. Monitoring and Profiling

### Per-Thread Statistics

```cpp
// Collect stats for a single thread
struct apth_thread_stats stats;
apth_get_thread_stats(thread_handle, &stats);

printf("Thread class=%d state=0x%x dispatches=%d "
       "cpu=%.3fs wall=%.3fs utilization=%.1f%%\n",
       stats.thread_class, stats.state, stats.dispatches,
       stats.cpu_time_sec, stats.wall_time_sec,
       (stats.cpu_time_sec / stats.wall_time_sec) * 100.0);
```

### Aggregate Statistics via `apth_for_each_thread()`

```cpp
struct AggregateStats {
    int total_threads;
    int waiting_threads;
    int running_threads;
    int dedicated_threads;
    double total_cpu_time;
    int total_dispatches;
};

static int stats_visitor(apth_t th, void *arg) {
    AggregateStats *agg = (AggregateStats *)arg;
    struct apth_thread_stats stats;
    if (apth_get_thread_stats(th, &stats) != 0) return 0;

    agg->total_threads++;
    agg->total_dispatches += stats.dispatches;
    agg->total_cpu_time += stats.cpu_time_sec;

    if (stats.state == APTH_THREAD_STATE_WAITING)
        agg->waiting_threads++;
    else if (stats.state == APTH_THREAD_STATE_RUNNING)
        agg->running_threads++;
    if (stats.thread_class == APTH_CLASS_DEDICATED)
        agg->dedicated_threads++;

    return 0;
}

void print_aggregate_stats() {
    AggregateStats agg = {};
    apth_for_each_thread(stats_visitor, &agg);

    printf("Threads: total=%d waiting=%d running=%d dedicated=%d\n",
           agg.total_threads, agg.waiting_threads,
           agg.running_threads, agg.dedicated_threads);
    printf("Total dispatches: %d, Total CPU time: %.3fs\n",
           agg.total_dispatches, agg.total_cpu_time);
}
```

### Worker Count and Capacity

```cpp
int workers = apth_get_worker_count();
// workers = number of scheduler pthreads = number of CPU cores available
// for M:N scheduling.
```

### NUMA Node Reporting

```cpp
#ifdef APTH_NUMA
int nodes = apth_get_numa_node_count();
printf("NUMA nodes: %d\n", nodes);

// Per-thread NUMA node
int node = apth_get_thread_numa_node(th);
// Returns -1 for dedicated threads (no scheduler binding)
printf("Thread on NUMA node: %d\n", node);
#endif
```

### JFR Integration Considerations

Java Flight Recorder (JFR) associates profiling events with OS thread IDs.
With LIBAPTH, multiple M:N threads share the same OS thread (scheduler
worker pthread). This means:

1. **CPU profiling samples** will be attributed to the worker pthread, not
   the individual apth. Use `apth_set_preempt_hook()` to collect per-apth
   CPU samples at preemption time.

2. **Thread event timestamps** (start, end, park, unpark) work correctly
   because JFR records them from the application's perspective, not the
   OS thread.

3. **Lock contention events** may show the worker pthread ID instead of
   the logical thread ID. Consider adding an apth-to-JavaThread mapping
   that JFR can query.

4. **Workaround**: For each JFR event, store the apth handle (or the
   JavaThread pointer) alongside the OS thread ID. Post-process JFR data
   to group events by apth rather than by OS thread.

---

## 10. Shutdown Sequence

### JVM Shutdown Flow

```
User calls System.exit(0) or last non-daemon thread exits:
  -> Threads::destroy_vm()
    -> 1. Wait for all non-daemon Java threads to exit
    ->    (each Java thread calls apth_exit() or returns from start func)
    -> 2. Run shutdown hooks (still in JVM main pthread context)
    -> 3. Terminate remaining daemon Java threads
    ->    (for each: send cancel signal, apth_join with timeout)
    -> 4. Stop GC threads (apth_join)
    -> 5. Stop VM thread (apth_join)
    -> 6. apth_drop()  <-- Tears down LIBAPTH
    -> 7. exit(0)
```

### `apth_drop()` Behavior

`apth_drop()` performs the following in order:

1. **Unblocks dedicated threads**: Closes the `dedicated_wake_fd` of all
   registered dedicated threads. If a dedicated thread is blocked on
   `read(wake_fd)`, it gets `EBADF` and can exit gracefully. This handles
   JIT compiler and watcher threads that may be sleeping.

2. **Stops the reactor**: The global reactor pthread exits its `epoll_wait`
   loop.

3. **Stops scheduler workers**: Sets `opening=false` for all workers. Each
   worker's scheduler loop exits, and the worker pthread terminates.
   `apth_global_scheduler_pool_drop()` joins all worker pthreads.

4. **Stops RDMA poller** (if active): Sets `running=false` and joins the
   poller pthread. (Note: the RDMA poller is stopped by setting its
   `running` flag; `apth_drop()` handles this implicitly through the
   scheduler pool teardown.)

5. **Cleans up subsystems**: Signal system, preemption timers, FD table,
   syscall wrappers.

### Code Example

```cpp
// File: src/hotspot/share/runtime/thread.cpp

void Threads::destroy_vm() {
    // ... existing shutdown logic ...

    // 1. Wait for all Java threads to exit
    wait_for_java_threads_to_exit();

    // 2. Join GC threads
    join_gc_threads();

    // 3. Join VM thread
    join_vm_thread();

    // 4. Tear down LIBAPTH
    //    This stops all scheduler workers, reactor, RDMA poller,
    //    and unblocks any remaining dedicated threads.
    apth_drop();

    // 5. At this point, all LIBAPTH state is cleaned up.
    //    Proceed with remaining JVM shutdown (JNI cleanup, etc.)
}
```

### Dedicated Thread Shutdown

When `apth_drop()` closes the `dedicated_wake_fd`:

- If a dedicated thread is blocked on `read(wake_fd)` (e.g., waiting in
  a sync primitive), the read returns with `EBADF`.
- The dedicated thread should handle this gracefully and exit.
- JVM dedicated threads (JIT, signal, watcher) typically have their own
  shutdown flags that are checked periodically. Set these flags before
  calling `apth_drop()` to ensure clean exit.

```cpp
// Before apth_drop():
jit_compiler->set_should_exit(true);     // JIT checks this in its compile loop
watcher_thread->set_should_exit(true);   // Watcher checks this in its timer loop
signal_dispatcher->set_should_exit(true);

// Give dedicated threads a moment to notice and exit
// (they will also get EBADF on wake_fd when apth_drop closes it)
apth_drop();
```

### Fallback for Hung Shutdown

If `apth_drop()` hangs (e.g., a thread is stuck in a long-running
syscall that does not check the shutdown flag):

```cpp
// Set a timeout
alarm(5);  // 5 second timeout
apth_drop();
alarm(0);  // Cancel if successful

// If alarm fires, the SIGALRM handler can call _exit(1)
// to force immediate process termination.
```

---

## 11. Known Limitations and Workarounds

### 1. `fork()` Not Supported After Init

LIBAPTH does not support `fork()` after initialization. The child process
inherits stale scheduler worker pthreads, a defunct reactor, and broken
eventfd state.

**JVM impact**: `Runtime.exec()` uses `fork()` + `exec()` (or `posix_spawn()`).

**Workaround**: Use `posix_spawn()` instead of `fork()` + `exec()`. OpenJDK
21 already defaults to `posix_spawn()` on Linux (see
`src/java.base/unix/native/libjava/ProcessImpl_md.c`). Verify that the
`-Djdk.lang.Process.launchMechanism=posix_spawn` property is set or that it
is the default on the target system.

### 2. `apth_drop()` May Hang

If threads are blocked in long-running syscalls (e.g., `accept()` on a
server socket with no incoming connections), `apth_drop()` may hang waiting
for scheduler workers to exit.

**Workaround**: Before calling `apth_drop()`, ensure all threads have
exited or are in a known-safe state. For server sockets, close the listening
FD to unblock `accept()`. As a last resort, use `_exit()` to terminate the
process.

### 3. FDs Shared Between Regular and Dedicated Threads

Regular M:N threads have their FDs set to `O_NONBLOCK` by LIBAPTH's I/O
hooks. Dedicated threads bypass the hooks and use blocking I/O. If a regular
thread opens a socket and a dedicated thread tries to use it (or vice versa),
the `O_NONBLOCK` state may be wrong.

**Workaround**: Create FDs from the thread type that will primarily use them.
If a socket is used by mutator threads (M:N), open it from an M:N thread
context. If a socket is used by the JIT compiler (dedicated), open it from
the dedicated thread. For FDs that must be shared, use `SYS_pipe2` or
`SYS_socket` directly with the desired flags.

### 4. JFR Profiling Data Needs Reinterpretation

Multiple M:N apths share the same OS thread (scheduler worker). JFR events
tagged with `os_thread_id` will group events from different Java threads
together.

**Workaround**: Store the `apth_t` handle or `JavaThread*` pointer with
each JFR event. Post-process JFR recordings to re-associate events with
logical threads. Alternatively, use `apth_get_thread_stats()` for per-thread
CPU time instead of JFR CPU profiling.

### 5. Stack Size Minimum

LIBAPTH enforces a minimum stack size of 16KB. The JVM's `-Xss` flag must
not be set below this. Guard pages are created automatically for all
M:N thread stacks.

**Recommendation**: Use `-Xss128k` or `-Xss256k` for Java threads. This
is much smaller than the default 1MB, saving memory when running thousands
of mutator threads. Stack overflow triggers SIGSEGV, which the JVM converts
to StackOverflowError.

### 6. NUMA Detection Requirements

NUMA topology detection (when compiled with `-DAPTH_NUMA`) reads from
`/sys/devices/system/node/`. If this filesystem is not available (e.g., in
a container without sysfs access), NUMA detection falls back to a single
node (UMA mode).

**Workaround**: Ensure the container has read access to
`/sys/devices/system/node/`. If not, LIBAPTH operates in UMA mode, which
is safe but suboptimal for multi-socket systems.

### 7. JNI Native Code

JNI methods run on the apth's stack and may call libc functions hooked by
LIBAPTH. This is usually transparent. However, long-running native methods
that never call any hooked function (pure computation) will not be
preempted in cooperative mode.

**Workaround**: `APTH_PREEMPT_SIGNAL` mode handles this: the SIGPROF timer
will fire even during JNI native code execution, setting the preemption
flag for the next yield opportunity.

### 8. Thread-Local Storage

LIBAPTH provides its own TLS (`apth_key_create`, `apth_getspecific`,
`apth_setspecific`) that follows the POSIX pthread_key semantics. The JVM
uses both `pthread_key_t` and C++ `thread_local` for thread-local data.

For M:N threads, `pthread_key_t`-based TLS returns the scheduler worker's
TLS (shared by all apths on that worker), NOT the individual apth's TLS.
Use `apth_key_create` / `apth_getspecific` / `apth_setspecific` instead.

C++ `thread_local` has the same problem: it is bound to the pthread, not
the apth. Replace any JVM code that uses `thread_local` for per-Java-thread
data with `apth_key_t`-based storage.

---

## 12. Complete Code Examples

### 12.1 Full `os::create_thread()` Replacement

```cpp
// File: src/hotspot/os/linux/os_linux.cpp

#include <apth.h>

extern "C" {
    // Forward declaration of the native thread entry point
    static void *thread_native_entry(void *arg);
}

static int apth_class_for(os::ThreadType thr_type) {
    switch (thr_type) {
    case os::java_thread:     return APTH_CLASS_IO_BOUND;
    case os::gc_thread:       return APTH_CLASS_DISTRIBUTED;
    case os::vm_thread:       return APTH_CLASS_CPU_BOUND;
    case os::compiler_thread: return APTH_CLASS_DEDICATED;
    case os::os_thread:       return APTH_CLASS_DEDICATED;
    case os::watcher_thread:  return APTH_CLASS_DEDICATED;
    default:                  return APTH_CLASS_DEFAULT;
    }
}

static const char *thread_type_name(os::ThreadType thr_type) {
    switch (thr_type) {
    case os::java_thread:     return "JVM-Mutator";
    case os::gc_thread:       return "JVM-GC-Worker";
    case os::vm_thread:       return "JVM-VM-Thread";
    case os::compiler_thread: return "JVM-JIT-Compiler";
    case os::os_thread:       return "JVM-Signal-Dispatcher";
    case os::watcher_thread:  return "JVM-Watcher";
    default:                  return "JVM-Thread";
    }
}

bool os::create_thread(Thread* thread, ThreadType thr_type,
                       size_t req_stack_size) {
    // Allocate OSThread
    OSThread* osthread = new OSThread();
    if (osthread == NULL) return false;
    thread->set_osthread(osthread);

    // Compute stack size
    size_t stack_size = req_stack_size;
    if (stack_size == 0) {
        switch (thr_type) {
        case os::java_thread:
            stack_size = JavaThread::stack_size_at_create();
            break;
        case os::compiler_thread:
            stack_size = 2 * 1024 * 1024;  // 2MB for JIT
            break;
        default:
            stack_size = 512 * 1024;  // 512KB default
            break;
        }
    }
    // Clamp to LIBAPTH minimum
    if (stack_size < 16 * 1024) stack_size = 16 * 1024;

    // Set up apth attributes
    int thread_class = apth_class_for(thr_type);

    apth_t tid;
    apth_attr_t attr;
    apth_attr_init(&attr);
    apth_attr_setstacksize(&attr, stack_size);
    apth_attr_setclass_np(&attr, thread_class);
    apth_attr_setname_np(&attr, thread_type_name(thr_type));

    // For signal dispatcher, set specific signal mask
    if (thr_type == os::os_thread) {
        sigset_t mask;
        sigemptyset(&mask);
        // Signal dispatcher should receive all signals
        apth_attr_setsigmask_np(&attr, &mask);
    }

    // Create the thread
    int ret = apth_create(&tid, &attr, thread_native_entry, thread);
    apth_attr_destroy(&attr);

    if (ret != 0) {
        thread->set_osthread(NULL);
        delete osthread;
        return false;
    }

    osthread->set_apth_id(tid);
    osthread->set_thread_type(thr_type);  // Store for later queries

    return true;
}
```

### 12.2 Full PlatformEvent Replacement

```cpp
// File: src/hotspot/os/posix/os_posix.cpp

#include <apth.h>

class PlatformEvent : public CHeapObj<mtSynchronizer> {
private:
    volatile int _event;
    apth_mutex_t _mutex;
    apth_cond_t  _cond;
    int          _nParked;  // For diagnostics

public:
    PlatformEvent() : _event(0), _nParked(0) {
        apth_mutex_init(&_mutex, NULL);
        apth_condattr_t cattr;
        apth_condattr_init(&cattr);
        apth_condattr_setclock(&cattr, CLOCK_MONOTONIC);
        apth_cond_init(&_cond, &cattr);
        apth_condattr_destroy(&cattr);
    }

    ~PlatformEvent() {
        apth_cond_destroy(&_cond);
        apth_mutex_destroy(&_mutex);
    }

    // Untimed park
    void park() {
        apth_mutex_lock(&_mutex);

        // Fast check: already unparked
        if (_event > 0) {
            _event = 0;
            apth_mutex_unlock(&_mutex);
            return;
        }

        _nParked++;
        while (_event <= 0) {
            apth_cond_wait(&_cond, &_mutex);
        }
        _nParked--;
        _event = 0;
        apth_mutex_unlock(&_mutex);
    }

    // Timed park (absolute time, CLOCK_MONOTONIC)
    int park(jlong millis) {
        apth_mutex_lock(&_mutex);

        if (_event > 0) {
            _event = 0;
            apth_mutex_unlock(&_mutex);
            return OS_OK;
        }

        struct timespec abstime;
        clock_gettime(CLOCK_MONOTONIC, &abstime);
        jlong nanos = millis * 1000000LL;
        abstime.tv_sec  += nanos / 1000000000LL;
        abstime.tv_nsec += nanos % 1000000000LL;
        if (abstime.tv_nsec >= 1000000000L) {
            abstime.tv_sec++;
            abstime.tv_nsec -= 1000000000L;
        }

        _nParked++;
        int status = OS_OK;
        while (_event <= 0) {
            int ret = apth_cond_timedwait(&_cond, &_mutex, &abstime);
            if (ret == ETIMEDOUT) {
                status = OS_TIMEOUT;
                break;
            }
        }
        _nParked--;

        if (_event > 0) {
            _event = 0;
            status = OS_OK;
        }

        apth_mutex_unlock(&_mutex);
        return status;
    }

    void unpark() {
        apth_mutex_lock(&_mutex);
        int s = _event;
        _event = 1;
        if (s < 1) {
            apth_cond_signal(&_cond);
        }
        apth_mutex_unlock(&_mutex);
    }

    void reset() { _event = 0; }
    int nParked() const { return _nParked; }
};
```

### 12.3 SafepointSynchronize Integration

```cpp
// File: src/hotspot/share/runtime/safepoint.cpp

#include <apth.h>

class ApthSafepointHelper {
public:
    // Bring all M:N threads to a halt.
    // After this call, all M:N threads have frozen stacks.
    static void pause_all_mn_threads() {
        int ret = apth_request_pause_all();
        assert(ret == 0, "apth_request_pause_all failed");
    }

    // Resume all M:N threads.
    static void resume_all_mn_threads() {
        int ret = apth_resume_all();
        assert(ret == 0, "apth_resume_all failed");
    }

    // Check if a thread is already at a safepoint.
    static bool is_at_safepoint(apth_t th) {
        int state = apth_getstate(th);
        // WAITING, READY, WAKED, NEW — all have frozen stacks
        // (after pause_all, no RUNNING threads exist among M:N threads)
        return state != APTH_THREAD_STATE_RUNNING &&
               state != APTH_THREAD_STATE_TERMINATED;
    }

    // Count threads by state (for diagnostics)
    struct StateCount {
        int waiting;
        int ready;
        int running;
        int other;
    };

    static StateCount count_thread_states() {
        StateCount sc = {};
        apth_for_each_thread([](apth_t th, void *arg) -> int {
            StateCount *sc = (StateCount *)arg;
            int state = apth_getstate(th);
            if (state == APTH_THREAD_STATE_WAITING)       sc->waiting++;
            else if (state == APTH_THREAD_STATE_READY)    sc->ready++;
            else if (state == APTH_THREAD_STATE_RUNNING)  sc->running++;
            else                                          sc->other++;
            return 0;
        }, &sc);
        return sc;
    }
};

void SafepointSynchronize::begin() {
    // Arm the safepoint flag (for dedicated threads and interpreter loop)
    _state = _synchronizing;
    OrderAccess::fence();

    // Pause all M:N threads. This blocks until no scheduler has a
    // RUNNING thread. Threads already in WAITING state (RDMA wait,
    // I/O wait, condvar wait) are instantly "at safepoint" — they have
    // frozen, walkable stacks.
    ApthSafepointHelper::pause_all_mn_threads();

    // Handle dedicated threads (JIT compiler, watcher, etc.)
    // They use existing safepoint polling mechanism.
    for (JavaThreadIterator jti; JavaThread *jt = jti.next(); ) {
        if (jt->osthread()->thread_type() == os::compiler_thread ||
            jt->osthread()->thread_type() == os::watcher_thread) {
            // Wait for these threads to reach safepoint poll
            while (!jt->is_at_safepoint()) {
                os::naked_yield();
            }
        }
    }

    _state = _synchronized;
    // All threads now at safepoint. GC can proceed.
}

void SafepointSynchronize::end() {
    _state = _not_synchronized;
    OrderAccess::fence();

    // Resume M:N threads
    ApthSafepointHelper::resume_all_mn_threads();

    // Dedicated threads resume via existing mechanism
    // (safepoint flag cleared, they leave their safepoint poll)
}
```

### 12.4 GC Root Scanning Loop

```cpp
// File: src/hotspot/share/gc/shared/gcRoots.cpp

#include <apth.h>

// Context for GC root scanning across all threads
struct GCRootContext {
    OopClosure *oops_closure;
    CodeBlobClosure *code_closure;
    int threads_scanned;
    int threads_skipped;
};

// Visitor callback for thread iteration during GC
static int gc_root_visitor(apth_t th, void *arg) {
    GCRootContext *ctx = (GCRootContext *)arg;

    // Map apth handle to JavaThread
    // (assumes JavaThread* stored in apth TLS key)
    struct apth_thread_stats stats;
    if (apth_get_thread_stats(th, &stats) != 0)
        return 0;

    // Skip terminated threads
    if (stats.state == APTH_THREAD_STATE_TERMINATED) {
        ctx->threads_skipped++;
        return 0;
    }

    // Get saved SP for stack walking
    void *saved_sp = NULL;
    int ret = apth_get_saved_sp(th, &saved_sp);

    if (ret == ENOTSUP) {
        // Dedicated thread: scan via standard pthread mechanism
        // (these threads should be at safepoint poll)
        ctx->threads_scanned++;
        return 0;  // Handled by existing dedicated thread scanning
    }

    if (ret == EBUSY) {
        // Thread is RUNNING (should not happen after pause_all)
        ctx->threads_skipped++;
        return 0;
    }

    if (ret != 0 || saved_sp == NULL) {
        ctx->threads_skipped++;
        return 0;
    }

    // Get stack bounds
    void *stack_base = NULL;
    size_t stack_size = 0;
    apth_get_stack_bounds(th, &stack_base, &stack_size);

    // Walk the stack from saved_sp to stack_top
    uintptr_t sp = (uintptr_t)saved_sp;
    uintptr_t stack_top = (uintptr_t)stack_base + stack_size;

    // Scan stack memory for oop references
    // (Simplified — real implementation uses frame walking)
    for (uintptr_t addr = sp; addr < stack_top; addr += sizeof(void*)) {
        oop *slot = (oop *)addr;
        if (is_valid_oop(*slot)) {
            ctx->oops_closure->do_oop(slot);
        }
    }

    ctx->threads_scanned++;
    return 0;
}

void gc_scan_thread_roots(OopClosure *oops, CodeBlobClosure *code) {
    GCRootContext ctx;
    ctx.oops_closure = oops;
    ctx.code_closure = code;
    ctx.threads_scanned = 0;
    ctx.threads_skipped = 0;

    // This must run during safepoint (after apth_request_pause_all)
    int total = apth_for_each_thread(gc_root_visitor, &ctx);

    log_trace(gc)("Root scan: total=%d scanned=%d skipped=%d",
                   total, ctx.threads_scanned, ctx.threads_skipped);
}
```

### 12.5 RDMA `fetch_remote_object`

```cpp
// File: src/hotspot/share/gc/rdma/rdmaBarrierSet.cpp

#include <apth.h>
#include <infiniband/verbs.h>
#include <string.h>

// Remote heap address range (VA-based detection)
#define REMOTE_HEAP_START 0x600000000000ULL
#define REMOTE_HEAP_END   0x700000000000ULL

static inline bool is_remote(oop obj) {
    uintptr_t addr = cast_from_oop<uintptr_t>(obj);
    return addr >= REMOTE_HEAP_START && addr < REMOTE_HEAP_END;
}

// Per-thread RDMA context (stored in JavaThread or apth TLS)
struct RDMAThreadCtx {
    struct ibv_qp *qp;        // Connection to memory node
    struct ibv_cq *send_cq;   // Send completion queue
    struct ibv_mr *local_mr;  // Memory region for local buffers
    uint64_t wr_id_counter;   // Monotonic WR ID
    // Local object cache (ring buffer)
    char *cache_buf;
    size_t cache_offset;
    size_t cache_size;
};

static RDMAThreadCtx *get_rdma_ctx() {
    JavaThread *jt = JavaThread::current();
    return jt->rdma_context();  // Stored in JavaThread
}

// Allocate space for a local copy from the per-thread cache
static void *alloc_local_copy(RDMAThreadCtx *ctx, size_t size) {
    // Simple bump allocator in per-thread cache
    size_t aligned_size = (size + 63) & ~63;  // Cache-line aligned
    if (ctx->cache_offset + aligned_size > ctx->cache_size) {
        ctx->cache_offset = 0;  // Wrap around (simple ring)
    }
    void *buf = ctx->cache_buf + ctx->cache_offset;
    ctx->cache_offset += aligned_size;
    return buf;
}

oop RDMABarrierSet::fetch_remote_object(oop remote_ref) {
    RDMAThreadCtx *ctx = get_rdma_ctx();

    // 1. Extract remote metadata
    uintptr_t remote_addr = cast_from_oop<uintptr_t>(remote_ref);
    // Object size is encoded in the remote pointer metadata or looked up
    // from a remote object table. For simplicity, assume fixed max size.
    size_t obj_size = get_remote_object_size(remote_ref);

    // 2. Allocate local buffer
    void *local_buf = alloc_local_copy(ctx, obj_size);

    // 3. Build scatter-gather element
    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uint64_t)(uintptr_t)local_buf;
    sge.length = (uint32_t)obj_size;
    sge.lkey = ctx->local_mr->lkey;

    // 4. Build RDMA read work request
    uint64_t wr_id = ctx->wr_id_counter++;
    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;
    wr.opcode = IBV_WR_RDMA_READ;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = get_remote_rkey_for_addr(remote_addr);

    // 5. Post RDMA read
    struct ibv_send_wr *bad_wr = NULL;
    int ret = ibv_post_send(ctx->qp, &wr, &bad_wr);
    guarantee(ret == 0, "ibv_post_send failed: errno=%d", errno);

    // 6. Wait for completion via LIBAPTH
    //
    //    Critical performance path:
    //    - Fast path (~10ns): ibv_poll_cq inside apth_rdma_wait finds
    //      the completion immediately (common for short RDMA RTT or
    //      when data was prefetched).
    //    - Slow path (~70ns): thread yields (~20ns), poller detects
    //      completion (~10-50ns), wakes scheduler (~20ns), thread
    //      resumes (~20ns).
    //    - Compare with pthread: ~5000ns (kernel context switch)
    //
    struct ibv_wc wc;
    ret = apth_rdma_wait(ctx->send_cq, wr_id, &wc);

    // 7. Check completion
    if (ret != 0 || wc.status != IBV_WC_SUCCESS) {
        // Log error and handle (retry, degrade to local copy, etc.)
        log_error(gc, rdma)("RDMA read failed: ret=%d status=%d wr_id=%lu",
                             ret, wc.status, wr_id);
        vm_exit_during_initialization("RDMA read failure");
    }

    // 8. The local buffer now contains a copy of the remote object.
    //    Update the object header to mark it as local (clear remote flag
    //    or adjust the pointer to the local heap).
    oop local_obj = cast_to_oop(local_buf);

    // Update forwarding pointer so future accesses go to the local copy.
    // This avoids redundant RDMA fetches for the same object.
    install_forwarding_pointer(remote_ref, local_obj);

    return local_obj;
}
```

---

## Appendix: HotSpot Files to Modify

| File | Changes |
|------|---------|
| `src/hotspot/os/linux/os_linux.cpp` | `os::init_2()` — add `apth_init_library()`. `os::create_thread()` — use `apth_create()` with class selection. `os::naked_yield()` — use `apth_yield()` for M:N threads. `os::sleep()` — hooked nanosleep for M:N threads. |
| `src/hotspot/os/linux/os_linux.hpp` | `OSThread` — add `apth_t _apth_id` field and accessors. |
| `src/hotspot/os/posix/os_posix.cpp` | `PlatformEvent` — replace pthread mutex/cond with apth mutex/cond. `PlatformMutex` / `PlatformMonitor` — replace with apth equivalents. |
| `src/hotspot/share/runtime/safepoint.cpp` | `SafepointSynchronize::begin/end` — integrate `apth_request_pause_all()` / `apth_resume_all()`. |
| `src/hotspot/share/runtime/thread.cpp` | `JavaThread::oops_do()` — use `apth_get_saved_sp()` for stack walking of yielded threads. |
| `src/hotspot/share/gc/rdma/` (new) | `RDMABarrierSet` — RDMA fetch barrier using `apth_rdma_wait()`. |
| `src/hotspot/cpu/x86/templateTable_x86.cpp` | `getfield` / `putfield` — add remote address range check for interpreted execution. |
| `src/hotspot/share/opto/` | `RDMABarrierSetC2` — JIT compiler barrier for remote object detection. |
| `make/hotspot/lib/CompileJvm.gmk` | Add `-lapth -libverbs` to `JVM_LIBS`, add include paths to `JVM_CFLAGS`. |

---

## Appendix: Thread Lifecycle Summary

```
JVM startup:
  main() [pthread]
    -> JNI_CreateJavaVM()
      -> Threads::create_vm()
        -> os::init_2()
          -> apth_init_library(N)       [starts N scheduler workers]
          -> (reactor starts automatically)
          -> (RDMA poller starts on first apth_rdma_register_cq)
        -> Create VM thread              [apth, APTH_CLASS_CPU_BOUND]
        -> Create GC threads             [apth, APTH_CLASS_DISTRIBUTED]
        -> Create JIT compiler threads   [apth, APTH_CLASS_DEDICATED]
        -> Create signal dispatcher      [apth, APTH_CLASS_DEDICATED]
        -> Create watcher thread         [apth, APTH_CLASS_DEDICATED]

Java Thread.start():
  -> JVM_StartThread()
    -> JavaThread::JavaThread()
      -> os::create_thread(java_thread)
        -> apth_create(APTH_CLASS_IO_BOUND)
          [assigned to scheduler via round-robin or current]

Object field access (interpreted getfield):
  load field value from object
    -> is_remote(value)?
       NO  -> return value                        [~1ns]
       YES -> RDMABarrierSet::fetch_remote_object(value)
         -> ibv_post_send(RDMA_READ)
         -> apth_rdma_wait(cq, wr_id, &wc)
           -> fast path: ibv_poll_cq hit          [~10ns]
           -> slow path: yield + poller + resume  [~70ns]
         -> return local copy

GC safepoint:
  SafepointSynchronize::begin()
    -> apth_request_pause_all()           [blocks until all M:N stopped]
    -> wait for dedicated threads         [existing safepoint poll]
    -> GC runs: scan stacks via apth_for_each_thread + apth_get_saved_sp
    -> SafepointSynchronize::end()
      -> apth_resume_all()

JVM shutdown:
  System.exit() -> Threads::destroy_vm()
    -> Wait for Java threads              [apth_join each]
    -> Join GC threads                    [apth_join each]
    -> Join VM thread                     [apth_join]
    -> apth_drop()                        [stops workers, reactor, poller]
    -> exit(0)
```
