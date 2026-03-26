# HotSpot JVM Modification Guide for LIBAPTH Integration

## Purpose of This Document

This document is a detailed technical specification for modifying the OpenJDK
HotSpot JVM to use LIBAPTH as its threading substrate and to add RDMA-based
disaggregated memory support.  It is written for an LLM coding assistant (or
human developer) who will implement these changes.

**Read this document entirely before making any changes.**

---

## 1. What Is LIBAPTH and Why Use It

### 1.1 What LIBAPTH Is

LIBAPTH is a userspace M:N threading library for x86-64 Linux.  It creates
many userspace threads ("apths") multiplexed onto a small number of OS
pthreads ("workers").  Key properties:

- **Userspace context switch** (~20ns): Uses hand-written x86-64 assembly
  (`src/internal/apth_ctx_x86_64.S`) to save/restore only callee-saved
  registers.  No syscalls during a context switch.  Compare with pthread
  context switch (~3-10μs, involves kernel scheduler).

- **I/O hook layer**: Interposes on libc functions (`read`, `write`, `accept`,
  `recv`, `send`, `open`, `close`, `select`, `poll`, etc.) via `dlsym(RTLD_NEXT)`.
  All FDs are set to `O_NONBLOCK`; when an operation returns `EAGAIN`, the
  thread yields to the scheduler and is woken when the FD becomes ready.

- **Global I/O reactor**: A dedicated pthread runs `epoll_wait` on a single
  epoll instance for all FDs.  Schedulers submit watch/unwatch requests to the
  reactor via a lock-protected queue.

- **RDMA completion poller**: When compiled with `-DAPTH_USE_RDMA`, a dedicated
  pthread busy-polls ibverbs completion queues (CQs).  `apth_rdma_wait(cq,
  wr_id, &wc)` yields the calling thread until the RDMA completion arrives.

- **Thread classification**: Threads can be `APTH_CLASS_IO_BOUND` (front of
  ready queue on wake) or `APTH_CLASS_CPU_BOUND` (back of queue, preemptible).

- **API compatibility**: API mirrors POSIX threads — `apth_create`,
  `apth_mutex_lock`, `apth_cond_wait`, etc.  Replacing `pthread_` with `apth_`
  and `PTHREAD_` with `APTH_` in source code is the primary mechanical change.

### 1.2 Why LIBAPTH for JVM on Disaggregated Memory

The target architecture has a compute node running the JVM and memory nodes
providing extra heap via RDMA.  When a JVM mutator thread accesses a remote
object, it issues an RDMA read and waits for the completion (5-50μs).

With NPTL (pthreads), this wait either busy-spins the CPU core or triggers a
kernel context switch (~5μs).  With hundreds of mutator threads, the kernel
scheduler becomes the bottleneck.

With LIBAPTH:
- RDMA wait yields in ~20ns (assembly context switch)
- Another mutator runs immediately on the same core
- The RDMA poller detects completion and wakes the original thread in ~50ns
- Total overhead: ~70ns vs ~5000ns for kernel scheduling
- Scales to thousands of mutator threads without kernel thrashing

### 1.3 LIBAPTH Source Layout

```
src/
├── apth.h                          # Public API header (install this)
├── common.h                        # Internal common definitions
├── internal/
│   ├── apth_ctx.h / .c             # Context switch (C wrapper)
│   ├── apth_ctx_x86_64.S           # Assembly context switch
│   ├── apth_sched.h / .c           # Scheduler loop
│   ├── apth_event.h / .c           # Event manager
│   ├── apth_reactor.h / .c         # Global I/O reactor
│   ├── apth_rdma.h / .c            # RDMA completion poller
│   ├── apth_tcb.h / .c             # Thread control block allocation
│   ├── apth_thqueue.h / .c         # Thread queue operations
│   ├── apth_worker.h / .c          # Worker pthread pool
│   ├── apth_fd.h / .c              # FD table management
│   ├── apth_preempt.h / .c         # Preemption timer
│   └── types/                      # Internal struct definitions
├── core/
│   ├── apth_init.c                 # Library initialization
│   ├── apth_create.c               # Thread creation
│   ├── apth_join.c / apth_exit.c   # Thread lifecycle
│   ├── apth_mutex.c / apth_cond.c  # Synchronization primitives
│   └── ...                         # Other POSIX-like APIs
├── hook_libc/                      # libc function interposition
├── attr/                           # Thread attribute functions
└── utils/                          # Utilities (debug, atomics, lists)
```

---

## 2. JVM Integration: Phase 3 — Threading Layer Replacement

### 2.1 Overview

The goal is to replace pthreads with LIBAPTH for **mutator threads and GC
threads** in HotSpot.  JVM-internal threads that must interact with the kernel
directly (signal dispatcher, JIT compiler) remain as raw pthreads.

### 2.2 Which JVM You Need

- **OpenJDK 17 LTS** or **OpenJDK 21 LTS** (both have well-documented
  `os_linux.cpp` threading layer)
- Clone from: `https://github.com/openjdk/jdk`
- The relevant files are in `src/hotspot/` subtree

### 2.3 File-by-File Changes

#### 2.3.1 `src/hotspot/os/linux/os_linux.cpp`

This is the main file that creates OS threads.  The key function is
`os::create_thread()`:

```cpp
// CURRENT CODE (simplified):
bool os::create_thread(Thread* thread, ThreadType thr_type, ...) {
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);
    int ret = pthread_create(&tid, &attr, thread_native_entry, thread);
    // ...
}
```

**CHANGE**: For mutator and GC threads, use `apth_create` instead:

```cpp
#include <apth.h>

// Determine if this thread type should use LIBAPTH
static bool use_apth(ThreadType thr_type) {
    switch (thr_type) {
    case os::java_thread:       // Mutator threads
    case os::gc_thread:         // GC workers
    case os::vm_thread:         // VM operations thread
        return true;
    case os::compiler_thread:   // JIT (CPU-bound, long-running)
    case os::os_thread:         // Signal dispatcher, etc.
    case os::watcher_thread:    // Periodic tasks
    default:
        return false;           // Keep as pthread
    }
}

bool os::create_thread(Thread* thread, ThreadType thr_type, ...) {
    if (use_apth(thr_type)) {
        apth_t tid;
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, stack_size);

        // Set thread class based on type
        if (thr_type == os::java_thread)
            apth_attr_setclass_np(&attr, APTH_CLASS_IO_BOUND);
        else if (thr_type == os::gc_thread)
            apth_attr_setclass_np(&attr, APTH_CLASS_CPU_BOUND);

        int ret = apth_create(&tid, &attr, thread_native_entry, thread);
        thread->osthread()->set_apth_id(tid);
        // ...
    } else {
        // Keep existing pthread_create path
        pthread_t tid;
        pthread_attr_t attr;
        // ...
    }
}
```

#### 2.3.2 `src/hotspot/os/linux/os_linux.hpp`

Add `apth_t` storage to `OSThread`:

```cpp
class OSThread : public CHeapObj<mtThread> {
    pthread_t _pthread_id;
    apth_t    _apth_id;      // ADD THIS
    bool      _is_apth;      // ADD THIS: true if this is a LIBAPTH thread
    // ...
public:
    void set_apth_id(apth_t id) { _apth_id = id; _is_apth = true; }
    apth_t apth_id() const { return _apth_id; }
    bool is_apth() const { return _is_apth; }
};
```

#### 2.3.3 `src/hotspot/os/linux/os_linux.cpp` — Thread Joining

The JVM joins threads in `os::terminate_thread()` and similar places:

```cpp
// CURRENT:
void os::terminate_thread(Thread* thread) {
    pthread_join(thread->osthread()->pthread_id(), NULL);
}

// CHANGE:
void os::terminate_thread(Thread* thread) {
    if (thread->osthread()->is_apth()) {
        apth_join(thread->osthread()->apth_id(), NULL);
    } else {
        pthread_join(thread->osthread()->pthread_id(), NULL);
    }
}
```

#### 2.3.4 `src/hotspot/os/linux/os_linux.cpp` — Thread Park/Unpark

HotSpot uses `PlatformEvent` (built on `futex`) for `Object.wait()`,
`Thread.sleep()`, and `LockSupport.park()`.  The futex-based implementation
blocks the entire OS thread.  Replace with apth condition variables:

```cpp
// File: src/hotspot/os/posix/os_posix.cpp or os_linux.cpp
// Class: PlatformEvent (or ParkEvent)

// CURRENT (simplified):
class PlatformEvent {
    volatile int _event;
    pthread_mutex_t _mutex;
    pthread_cond_t _cond;
public:
    void park() {
        pthread_mutex_lock(&_mutex);
        while (_event <= 0)
            pthread_cond_wait(&_cond, &_mutex);
        _event = 0;
        pthread_mutex_unlock(&_mutex);
    }
    void unpark() {
        pthread_mutex_lock(&_mutex);
        _event = 1;
        pthread_cond_signal(&_cond);
        pthread_mutex_unlock(&_mutex);
    }
};

// CHANGE: dual-mode implementation
class PlatformEvent {
    volatile int _event;
    // Both implementations exist; choose at runtime based on thread type
    pthread_mutex_t _pthread_mutex;
    pthread_cond_t  _pthread_cond;
    apth_mutex_t    _apth_mutex;
    apth_cond_t     _apth_cond;
    bool            _use_apth;

public:
    void init(bool use_apth) {
        _use_apth = use_apth;
        _event = 0;
        if (_use_apth) {
            apth_mutex_init(&_apth_mutex, NULL);
            apth_cond_init(&_apth_cond, NULL);
        } else {
            pthread_mutex_init(&_pthread_mutex, NULL);
            pthread_cond_init(&_pthread_cond, NULL);
        }
    }

    void park() {
        if (_use_apth) {
            apth_mutex_lock(&_apth_mutex);
            while (_event <= 0)
                apth_cond_wait(&_apth_cond, &_apth_mutex);
            _event = 0;
            apth_mutex_unlock(&_apth_mutex);
        } else {
            pthread_mutex_lock(&_pthread_mutex);
            while (_event <= 0)
                pthread_cond_wait(&_pthread_cond, &_pthread_mutex);
            _event = 0;
            pthread_mutex_unlock(&_pthread_mutex);
        }
    }

    void unpark() {
        if (_use_apth) {
            apth_mutex_lock(&_apth_mutex);
            _event = 1;
            apth_cond_signal(&_apth_cond);
            apth_mutex_unlock(&_apth_mutex);
        } else {
            pthread_mutex_lock(&_pthread_mutex);
            _event = 1;
            pthread_cond_signal(&_pthread_cond);
            pthread_mutex_unlock(&_pthread_mutex);
        }
    }
};
```

#### 2.3.5 `src/hotspot/os/linux/os_linux.cpp` — Mutex/Monitor

HotSpot uses `os::PlatformMutex` and `os::PlatformMonitor` for internal VM
locks.  These wrap `pthread_mutex_t` / `pthread_cond_t`.

```cpp
// CURRENT:
class PlatformMutex : public CHeapObj<mtSynchronizer> {
    pthread_mutex_t _mutex;
};

// CHANGE: For locks that may be held by apth threads, use apth_mutex.
// For locks only held by pthread threads (JIT, signals), keep pthread.

class PlatformMutex : public CHeapObj<mtSynchronizer> {
    union {
        pthread_mutex_t _pthread_mutex;
        apth_mutex_t    _apth_mutex;
    };
    bool _use_apth;
    // ...
};
```

**IMPORTANT**: A mutex must use the same implementation for ALL threads that
may lock/unlock it.  If both apth threads and pthread threads contend on the
same mutex, you MUST use pthread (because apth_mutex_lock calls apth_yield
which only works from apth context).  Identify which locks are "apth-only"
vs "mixed" and configure accordingly.

#### 2.3.6 `src/hotspot/os/linux/os_linux.cpp` — `os::yield()`

```cpp
// CURRENT:
void os::naked_yield() {
    sched_yield();
}

// CHANGE:
void os::naked_yield() {
    Thread* current = Thread::current_or_null();
    if (current && current->osthread() && current->osthread()->is_apth()) {
        apth_yield();   // Userspace yield (~20ns)
    } else {
        sched_yield();  // Kernel yield (~3μs)
    }
}
```

#### 2.3.7 `src/hotspot/os/linux/os_linux.cpp` — `os::sleep()`

```cpp
// CURRENT:
int os::sleep(Thread* thread, jlong millis) {
    struct timespec ts;
    ts.tv_sec = millis / 1000;
    ts.tv_nsec = (millis % 1000) * 1000000;
    nanosleep(&ts, NULL);  // Blocks entire OS thread
    return OS_OK;
}

// CHANGE: Use apth_cond_timedwait for apth threads (yields to scheduler)
int os::sleep(Thread* thread, jlong millis) {
    if (thread->osthread()->is_apth()) {
        // Use LIBAPTH's hooked nanosleep, which yields to scheduler
        struct timespec ts;
        ts.tv_sec = millis / 1000;
        ts.tv_nsec = (millis % 1000) * 1000000;
        nanosleep(&ts, NULL);  // Hooked by LIBAPTH: yields, resumes after timeout
        return OS_OK;
    }
    // Fallback for pthread threads
    // ...
}
```

Note: LIBAPTH already hooks `nanosleep` / `usleep` / `sleep`, so if using
`LD_PRELOAD`, this change may not be needed.  With direct integration, ensure
the hooked version is called.

### 2.4 Build System Changes

In the OpenJDK build system (`make/autoconf/` or `configure`):

```bash
# Add to configure flags:
./configure \
    --with-extra-cflags="-DAPTH_USE_RDMA" \
    --with-extra-cxxflags="-DAPTH_USE_RDMA" \
    --with-extra-ldflags="-L/path/to/libapth/build/lib -lapth -libverbs"

# Or modify make/hotspot/lib/CompileJvm.gmk to add:
JVM_LIBS += -lapth -libverbs -luring
JVM_CFLAGS += -I/path/to/libapth/src -DAPTH_USE_RDMA
```

### 2.5 Library Initialization

LIBAPTH must be initialized before any apth threads are created.  The JVM's
`main()` calls `Threads::create_vm()` which eventually calls
`os::init_2()`.  Add LIBAPTH init here:

```cpp
// In src/hotspot/os/linux/os_linux.cpp:
jint os::init_2(void) {
    // ... existing initialization ...

    // Initialize LIBAPTH
    apth_init_t initvals;
    apth_config_defaults(&initvals);
    initvals.workers = os::active_processor_count();  // One worker per core
    // NOTE: Don't set main_apth here — the JVM's main thread is a pthread.
    // LIBAPTH is initialized for worker threads only.

    // ... or use the APTH_CONFIG macro in a separate translation unit ...

    return JNI_OK;
}
```

**IMPORTANT**: The JVM's main thread is NOT an apth.  It's the original
pthread that called `main()`.  Only threads created via `os::create_thread()`
with `use_apth() == true` are apths.  This means `apth_init()` should be
called with a configuration that spawns worker pthreads but does NOT replace
the main thread.  Use `apth_global_scheduler_pool_init()` directly if needed
for more control.

---

## 3. JVM Integration: Phase 4 — RDMA Object Access Barrier

### 3.1 Overview

The object access barrier is the mechanism that detects when a Java object is
on remote memory and transparently fetches it via RDMA.  This is the most
performance-critical piece: it runs on EVERY object field access.

### 3.2 Object Layout

In disaggregated memory, a Java object can be:
- **Local**: in the compute node's local heap (fast access)
- **Remote**: on a memory node (requires RDMA fetch)

You need a bit in the object header (or a separate data structure) to
distinguish local from remote.  Options:

**Option A: Header bit** (fast check, wastes 1 bit in every object header)
```
// HotSpot object header (markWord, 64-bit):
// Bits 63-3: hash, age, lock state, etc.
// Bit 2: remote flag (NEW)
// Bits 1-0: lock bits

static inline bool is_remote(oop obj) {
    return (obj->mark().value() & 0x4) != 0;
}
```

**Option B: Address range** (zero per-object overhead)
```
// Reserve a VA range for remote objects.  Any pointer in this range
// is remote.  Requires mmap'ing a fixed-address range.
#define REMOTE_HEAP_START 0x600000000000ULL
#define REMOTE_HEAP_END   0x700000000000ULL

static inline bool is_remote(oop obj) {
    uintptr_t addr = (uintptr_t)obj;
    return addr >= REMOTE_HEAP_START && addr < REMOTE_HEAP_END;
}
```

Option B is preferred: zero per-object overhead, simple range check.

### 3.3 Barrier Implementation

#### 3.3.1 Where Barriers Are Inserted

HotSpot's `BarrierSet` framework controls where memory access barriers are
inserted.  The relevant files:

```
src/hotspot/share/gc/shared/barrierSet.hpp       # Abstract barrier interface
src/hotspot/share/gc/shared/barrierSetAssembler.hpp  # JIT barrier codegen
src/hotspot/share/oops/access.hpp                # Unified access API
src/hotspot/share/oops/accessBackend.hpp         # Access backend
src/hotspot/cpu/x86/gc/shared/barrierSetAssembler_x86.cpp  # x86 JIT barriers
```

#### 3.3.2 Create a New BarrierSet

Create `RDMABarrierSet` that wraps the existing GC barrier and adds RDMA
fetch logic:

```
src/hotspot/share/gc/rdma/rdmaBarrierSet.hpp
src/hotspot/share/gc/rdma/rdmaBarrierSet.cpp
src/hotspot/share/gc/rdma/rdmaBarrierSet.inline.hpp
src/hotspot/cpu/x86/gc/rdma/rdmaBarrierSetAssembler_x86.hpp
src/hotspot/cpu/x86/gc/rdma/rdmaBarrierSetAssembler_x86.cpp
```

```cpp
// rdmaBarrierSet.hpp
class RDMABarrierSet : public BarrierSet {
    // Wraps another BarrierSet (e.g., G1BarrierSet) and adds RDMA fetch
    BarrierSet* _inner;

    // RDMA state
    struct ibv_pd *_pd;          // Protection domain
    struct ibv_cq *_cq;          // Completion queue (per-thread or shared)
    struct ibv_qp **_qps;        // Queue pairs (per-connection)

public:
    // The critical hot-path function:
    template <typename T>
    oop oop_load_at(oop base, ptrdiff_t offset) {
        oop result = _inner->oop_load_at(base, offset);
        if (is_remote(result)) {
            result = fetch_remote_object(result);
        }
        return result;
    }

    oop fetch_remote_object(oop remote_ref);
};
```

```cpp
// rdmaBarrierSet.cpp
oop RDMABarrierSet::fetch_remote_object(oop remote_ref) {
    // 1. Determine remote address and size from the object metadata
    uintptr_t remote_addr = get_remote_addr(remote_ref);
    size_t obj_size = get_object_size(remote_ref);

    // 2. Allocate local buffer for the fetched object
    void *local_buf = allocate_local_copy(obj_size);

    // 3. Post RDMA read
    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uint64_t)(uintptr_t)local_buf;  // Use as identifier
    wr.opcode = IBV_WR_RDMA_READ;
    wr.sg_list = &sge;  // Points to local_buf
    wr.num_sge = 1;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = get_remote_rkey(remote_ref);

    struct ibv_send_wr *bad_wr;
    ibv_post_send(get_qp_for_thread(), &wr, &bad_wr);

    // 4. Wait for RDMA completion using LIBAPTH
    //    This yields the mutator thread (~20ns) and lets other mutators
    //    run on the same core while RDMA is in flight (~5-50μs).
    struct ibv_wc wc;
    apth_rdma_wait(get_cq_for_thread(), wr.wr_id, &wc);

    // 5. Check completion status
    if (wc.status != IBV_WC_SUCCESS) {
        // Handle error (retry, signal OOM, etc.)
        fatal("RDMA read failed: status=%d", wc.status);
    }

    // 6. Return the local copy as a local oop
    return cast_to_oop(local_buf);
}
```

#### 3.3.3 Template Interpreter Changes

For interpreted execution, add the RDMA check to `getfield` / `putfield`
bytecodes:

```
src/hotspot/cpu/x86/templateInterpreterGenerator_x86.cpp
src/hotspot/cpu/x86/templateTable_x86.cpp
```

In `TemplateTable::getfield()`:

```cpp
// After loading the field value:
//   mov rax, [obj + offset]   ; load field

// ADD: Remote check
//   cmp rax, REMOTE_HEAP_START
//   jb  local_access
//   cmp rax, REMOTE_HEAP_END
//   jae local_access
//   ; --- Remote path ---
//   call rdma_fetch_stub       ; C++ stub that calls apth_rdma_wait
// local_access:
```

The remote check is 2 comparisons + 1 branch, which is ~2ns overhead on the
fast path (local objects).

#### 3.3.4 C2 JIT Compiler Changes

For JIT-compiled code, the C2 compiler must emit the RDMA barrier at object
load sites.  Modify the ideal graph to insert a barrier node:

```
src/hotspot/share/opto/library_call.cpp
src/hotspot/share/opto/compile.cpp
src/hotspot/share/opto/graphKit.cpp
```

The C2 `BarrierSetC2` interface is the right place:

```cpp
// In rdmaBarrierSetC2.cpp:
Node* RDMABarrierSetC2::load_at_resolved(C2Access& access, ...) {
    Node* load = BarrierSetC2::load_at_resolved(access, ...);

    // Insert a conditional RDMA fetch after the load:
    // if (load >= REMOTE_HEAP_START && load < REMOTE_HEAP_END)
    //     load = rdma_fetch(load);

    Node* cmp_lo = gvn.transform(new CmpPNode(load, remote_heap_start));
    Node* bol_lo = gvn.transform(new BoolNode(cmp_lo, BoolTest::ge));
    // ... build the diamond control flow ...

    return phi;  // Merge of local-path and remote-path
}
```

### 3.4 GC Integration for RDMA

#### 3.4.1 Safepoint Handling

The GC coordinator calls `SafepointSynchronize::begin()` which sets a
safepoint flag and waits for all Java threads to reach a safepoint.

With LIBAPTH, threads in `APTH_STATE_WAITING` (yielded for RDMA or I/O) have
frozen stacks that are safe to scan.  Modify the safepoint mechanism to treat
these as "at safepoint":

```cpp
// In src/hotspot/share/runtime/safepoint.cpp:
bool SafepointSynchronize::is_at_safepoint(JavaThread* thread) {
    // Existing checks...
    if (thread->is_apth_thread()) {
        apth_t at = thread->osthread()->apth_id();
        // An apth in WAITING state has a frozen, scannable stack
        if (apth_state(at) == APTH_STATE_WAITING)
            return true;
    }
    // ...
}
```

Note: You may need to add `apth_state()` to the LIBAPTH public API:
```c
// In apth.h:
int apth_getstate(apth_t th);  // Returns APTH_STATE_* constant
```

#### 3.4.2 GC Root Scanning

The GC needs to scan thread stacks for object references.  With LIBAPTH,
the stack walker must use the thread's saved stack pointer (not the running
pthread's stack):

```cpp
// In src/hotspot/share/runtime/thread.cpp:
void JavaThread::oops_do(OopClosure* f) {
    if (is_apth_thread() && /* thread is yielded */) {
        // Stack is saved in the apth's context structure.
        // Walk from the saved SP to the stack base.
        frame fr = /* reconstruct frame from apth's saved SP */;
        // ... walk frames and apply closure f ...
    } else {
        // Existing stack walking for running threads
    }
}
```

### 3.5 Thread Lifecycle Summary

```
JVM startup:
  main() → JNI_CreateJavaVM() → Threads::create_vm()
    → os::init_2()
      → Initialize LIBAPTH (apth_global_scheduler_pool_init)
      → apth_reactor_start()
      → apth_rdma_poller_start() [if RDMA enabled]
    → Create VM thread (apth, APTH_CLASS_DEFAULT)
    → Create GC threads (apth, APTH_CLASS_CPU_BOUND)
    → Create compiler threads (pthread, kept as-is)

Java thread creation (from Thread.start()):
  java.lang.Thread.start()
    → JVM_StartThread()
      → JavaThread::JavaThread()
        → os::create_thread(java_thread)
          → apth_create(&tid, &attr, thread_native_entry, thread)
            [attr has APTH_CLASS_IO_BOUND for mutators]

Remote object access (from bytecode getfield):
  interpreter or JIT: load field → is_remote(ref)?
    YES → rdmaBarrierSet::fetch_remote_object(ref)
      → ibv_post_send(qp, &rdma_read_wr, ...)
      → apth_rdma_wait(cq, wr_id, &wc)
        → [fast path: ibv_poll_cq succeeds immediately, ~10ns]
        → [slow path: yield to scheduler ~20ns, poller detects completion,
           wake scheduler, resume thread ~50ns total]
      → return local copy of object
    NO → normal field access (~1ns)

GC safepoint:
  GC coordinator: SafepointSynchronize::begin()
    → Set safepoint flag
    → For each Java thread:
      → If thread is running: wait for it to reach safepoint poll
      → If thread is APTH_STATE_WAITING: treat as at safepoint (stack frozen)
    → GC runs: scan stacks, mark, compact/evacuate
    → SafepointSynchronize::end()
      → Clear flag, resume all threads

JVM shutdown:
  System.exit() → Threads::destroy_vm()
    → Join all Java threads
    → apth_rdma_poller_stop()
    → apth_reactor_stop()
    → apth_drop()
```

---

## 4. Testing Strategy

### 4.1 Unit Tests (before JVM integration)

1. Compile LIBAPTH with RDMA enabled:
   ```bash
   # In LIBAPTH Makefile, add -DAPTH_USE_RDMA to CFLAGS and -libverbs to LDFLAGS
   make clean && make all
   ```

2. Write a standalone test that simulates the JVM workload:
   ```c
   // test_rdma_simulation.c
   // - Create N apth threads (simulating mutators)
   // - Each thread does a loop of: compute → RDMA wait → compute
   // - Measure throughput vs pthread baseline
   ```

3. Run existing LIBAPTH tests to verify no regressions:
   ```bash
   make run-io-apth-tests
   ```

### 4.2 JVM Integration Tests

1. **Minimal boot**: `java -version` with LIBAPTH
2. **Hello world**: `java HelloWorld`
3. **DaCapo benchmark suite**: Stress-tests threading, GC, JIT
4. **SPECjbb**: Server-side benchmark with many threads
5. **Custom RDMA test**: Java application that accesses remote objects

### 4.3 Correctness Validation

- Run with `-XX:+VerifyStack` to validate stack walking
- Run with `-Xcheck:jni` to catch JNI issues
- Use ThreadSanitizer (TSan) to detect data races
- Test safepoints with forced GC (`System.gc()` in a loop)

---

## 5. Performance Tuning

### 5.1 LIBAPTH Configuration

```bash
# Recommended compile flags for JVM use:
CFLAGS += -DAPTH_CUR_USING_KEYWORD \
          -DAPTH_HOLD_INITIALIZER_PTHREAD \
          -DAPTH_PREEMPT_SIGNAL \
          -DAPTH_PREEMPT_QUANTUM_MS=5 \
          -DAPTH_NUMA \
          -DAPTH_USE_IOURING \
          -DAPTH_USE_RDMA
```

### 5.2 JVM Flags

```bash
java \
    -XX:+UseG1GC \
    -XX:ParallelGCThreads=4 \
    -XX:ConcGCThreads=2 \
    -Xss128k \               # Smaller stacks (LIBAPTH handles guard pages)
    -Xmx4g \
    MyApplication
```

### 5.3 Key Metrics to Monitor

- **RDMA fast-path hit rate**: What % of `apth_rdma_wait` calls complete on
  the first `ibv_poll_cq`? Target: >50% for good prefetching.
- **Context switches per second**: `apth_get_stats()` dispatches counter.
  Target: <100K/s per core for efficient scheduling.
- **GC pause time**: Safepoint latency should not increase. Waiting threads
  are already at safepoint, so LIBAPTH should improve this.
- **RDMA queue depth**: Monitor CQ utilization. If CQs are always empty,
  prefetching is effective. If always full, the network is saturated.

---

## 6. Known Limitations and Workarounds

1. **JNI native code**: JNI methods run on the apth's stack but may call libc
   functions that are hooked by LIBAPTH. This is usually fine, but long-running
   native methods that don't call any hooked function will not be preempted.
   Workaround: use `APTH_PREEMPT_SIGNAL` mode.

2. **`fork()` after LIBAPTH init**: The child process will have stale
   scheduler/reactor threads. LIBAPTH does not support fork. The JVM rarely
   forks (only in `Runtime.exec()`), so this is usually not an issue.

3. **Mixed pthread/apth locks**: Some JVM internal locks may be held by both
   pthread and apth threads. These must use pthread mutexes (not apth mutexes)
   because `apth_mutex_lock` calls `apth_yield` which only works from apth
   context.

4. **JFR/JMX profiling**: Java Flight Recorder and JMX use OS-level thread
   IDs for profiling. With LIBAPTH, multiple apths share the same OS thread.
   Profiling data may need reinterpretation. Consider adding LIBAPTH-aware
   profiling hooks.

5. **Stack size**: Default JVM thread stack is 1MB. With 1000 mutator threads,
   that's 1GB of stacks. Consider reducing to 128KB-256KB since LIBAPTH
   provides guard page protection.
