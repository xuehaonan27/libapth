# LIBAPTH: A Userspace Thread Library

LIBAPTH is a userspace thread library designed to minimize kernel-userspace
context switches and maximize I/O efficiency. It creates multiple user-space
scheduled threads (apths) on top of several pthread-based workers.

Applications built with LIBAPTH can achieve better performance in I/O-bound
workloads by reducing unnecessary context switches and keeping the CPU busy
with useful work instead of waiting for I/O operations to complete.

**Design Goals**:
1. Minimize kernel-userspace context switches
2. Maximize I/O efficiency
3. Provide pthread-compatible API

## Architecture

```
                ┌──────────────────────────────────────────────┐
                │                 User Code                    │
                │          (apth_create, read, write, ...)     │
                └─────────────┬────────────────────────────────┘
                              │
                ┌─────────────▼────────────────────────────────┐
                │              LIBAPTH Core                     │
                │                                              │
                │  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
                │  │Scheduler │  │Scheduler │  │Scheduler │   │
                │  │ Worker 0 │  │ Worker 1 │  │ Worker N │   │
                │  │ (pthread)│  │ (pthread)│  │ (pthread)│   │
                │  └────┬─────┘  └────┬─────┘  └────┬─────┘   │
                │       │             │              │         │
                │       └──────┬──────┘──────────────┘         │
                │              │ wake_eventfd                  │
                │     ┌────────▼─────────┐                     │
                │     │  Global Reactor  │                     │
                │     │    (pthread)     │                     │
                │     │   epoll_wait     │                     │
                │     └──────────────────┘                     │
                └──────────────────────────────────────────────┘
```

**Schedulers** are pure compute dispatchers, each running on a dedicated
pthread worker. They manage thread queues (new, ready, waiting, waked,
running, terminated) and handle non-I/O events (timers, signals, thread
joins, condition variables).

**Global Reactor** is a dedicated pthread that owns a single epoll instance
for all file descriptor I/O. Schedulers submit FD wait requests to the
reactor via a lock-protected MPSC queue. When FDs become ready, the reactor
marks events as occurred and wakes the appropriate scheduler. This
decouples I/O event management from scheduling and eliminates per-scheduler
epoll overhead.

**Work Stealing** allows idle schedulers to steal threads from other
schedulers' ready queues to balance load.

**Hybrid Scheduling** (`APTH_CLASS_DEDICATED`) allows specific threads to
run as 1:1 dedicated pthreads that bypass the userspace scheduler entirely.
These threads use blocking kernel I/O instead of hooked nonblocking I/O,
and block on an eventfd instead of yielding when contending on sync
primitives. They can share mutexes, condition variables, barriers,
semaphores, and rwlocks with regular M:N apths. See
`docs/hybrid_scheduling.md` for full details. Key files:
- `src/core/apth_dedicated.c` / `src/internal/apth_dedicated.h` — dedicated
  thread wrapper, exit path, block/unblock helpers
- `src/core/apth_create.c` — dedicated creation path (pthread_create instead
  of scheduler queue)
- All sync primitives in `src/core/apth_*.c` have dedicated-aware wait/wake
- All I/O hooks in `src/hook_libc/` bypass to raw syscalls for dedicated threads

**JVM Integration APIs** provide everything needed for HotSpot integration:
- `apth_init_library(workers)` — library-mode init without APTH_MAIN_BEGIN
- `apth_getstate(th)` — query thread state (for GC safepoints)
- `apth_get_saved_sp(th, &sp)` — get frozen SP (for GC stack walking)
- `apth_get_stack_bounds(th, &base, &size)` — stack memory range
- `apth_for_each_thread(visitor, arg)` — iterate all live threads
- `apth_request_pause_all()` / `apth_resume_all()` — global scheduler pause
- `apth_set_state_callback(cb, arg)` — thread state change notifications
- `apth_set_preempt_hook(hook, arg)` — hook into preemption check points
- `apth_get_thread_stats(th, &stats)` — per-thread dispatch/timing stats
- `apth_get_worker_count()` — query number of scheduler workers
- `APTH_CLASS_DISTRIBUTED` — round-robin scheduler assignment for GC workers
- NUMA-aware work stealing and thread placement (`APTH_NUMA`)
- See `docs/jvm_integration_guide.md` for the complete 12-section guide

## Compile and Install
```shell
make all
sudo make install
# Use LIBAPTH by including <apth.h> instead of <pthread.h>
```

## Quick Start
The only thing different when using LIBAPTH is main function:
```C
// Configure total workers here. Gain better performance if the worker number
// is less than or equal to available CPU cores.
APTH_CONFIG(cfg, cfg->workers = 4;)

// Main entry. `argc` and `argv` could be changed to other names you like.
APTH_MAIN_BEGIN(argc, argv)
{
    // Write your main function
}
APTH_MAIN_END //
```

## Key Points
1. LIBAPTH is a userspace thread library. Userspace threads in LIBAPTH are
called `apth`s. LIBAPTH is **NOT** a coroutine or asynchronous I/O library.
+ Provides API mimicking GNU NPTL (Pthread). Theoretically, just open your
favorite editor, replace each `pthread_` with `apth_`, `PTHREAD_` with `APTH_`,
 and of course `pthread.h` with `apth.h`, and the program will still compile
and run, with an improved performance.
+ Aiming at transforming Pthread based program into a user space scheduled one
with improved performance. LIBAPTH covers up I/O wait time as much as it can.
+ All `apth`s are scheduled in userspace, so a lot of kernel-user context
switches are mitigated. Workers (kernel threads, which are Pthreads on POSIX
platform) that carry workloads (`apth`s) are 1:1 bound to CPU cores.

## Limitations
1. Currently only supports x86-64 Linux platform. And LIBAPTH semantics come
most from GNU LIBC. BSD Unix and MUSL semantics not considered.
2. Fork/exec and process creation APIs are currently unsupported.

## Conditional Compilation Flags

### Core Flags

| Flag | Default | Description |
|------|---------|-------------|
| `APTH_HOLD_INITIALIZER_PTHREAD` | **Defined** | Hold the initializer pthread, waiting to join the first scheduler worker. When set, the library calls `exit()` after worker 0 finishes; otherwise it calls `pthread_exit()` and lets other pthreads continue. Enabled by default in the Makefile. |
| `APTH_CUR_USING_KEYWORD` | **Defined** | Use C11 `_Thread_local` keyword for scheduler TLS instead of `pthread_getspecific`/`pthread_setspecific`. Faster but requires compiler support. Enabled by default in the Makefile. |
| `APTH_STACKGROWTH` | `-1` | Stack growth direction. `-1` means downward (high to low), which is the default on x86-64. Set to a positive value on platforms with upward-growing stacks. |
| `APTH_BUILDING_DLL` | *Undefined* | Define when building LIBAPTH as a shared library (DLL) to enable `__attribute__((visibility("default")))` on exported symbols. |

### Preemption Flags

LIBAPTH supports three preemption modes, selected at compile time. At most
one of `APTH_PREEMPT_SIGNAL` and `APTH_PREEMPT_INSTRUMENT` may be defined;
defining both is a compile-time error. If neither is defined, cooperative
scheduling is used.

| Flag | Description |
|------|-------------|
| *(neither defined)* | **Cooperative scheduling** (default). Threads must explicitly yield or call a hooked I/O function to relinquish the CPU. A CPU-bound thread that never does I/O will starve other threads on its scheduler. Suitable for I/O-heavy servers. |
| `APTH_PREEMPT_SIGNAL` | **Signal-based preemption**. Each scheduler sets a per-worker `SIGPROF` timer that fires every `APTH_PREEMPT_QUANTUM_MS` milliseconds. The signal handler sets a flag; the next hooked function or cancellation point checks the flag and yields. Provides fairness for CPU-bound threads at the cost of one timer signal per quantum. |
| `APTH_PREEMPT_INSTRUMENT` | **Compiler instrumentation preemption**. Requires compiling user code with `-finstrument-functions` (GCC/Clang). A hook at every function entry checks a counter and yields when the thread has exceeded its time quantum. Deterministic and signal-free, but requires recompilation of user code. |
| `APTH_PREEMPT_QUANTUM_MS` | Preemption time quantum in milliseconds (default: `10`). Used by `APTH_PREEMPT_SIGNAL` mode. |
| `APTH_PREEMPT_INSTRUMENT_THRESHOLD` | Function-entry count threshold for instrumentation mode (default: `10000`). After this many function entries, the thread yields. Used by `APTH_PREEMPT_INSTRUMENT` mode. |

#### Compiler Instrumentation Preemption Guide

The `APTH_PREEMPT_INSTRUMENT` mode uses GCC/Clang's `-finstrument-functions`
flag to insert a preemption check at every function entry in user code. This
is deterministic and signal-free, but requires both the library and user code
to be compiled with specific flags.

**How it works:**

When a C/C++ file is compiled with `-finstrument-functions`, the compiler
inserts calls to `__cyg_profile_func_enter` at the beginning of every
function. LIBAPTH provides this hook (as a weak symbol): it increments a
per-worker counter and yields to the scheduler when the counter exceeds
`APTH_PREEMPT_INSTRUMENT_THRESHOLD` (default 10000 function entries). This
gives CPU-bound threads a fair share of execution time without relying on
signals or timers.

**Step 1: Build LIBAPTH with instrumentation preemption**

Edit the `Makefile` to replace the preemption flag (remove
`-DAPTH_PREEMPT_SIGNAL` if present, they are mutually exclusive):

```makefile
CFLAGS := -Wall -Wextra -std=gnu11 -g -O2 -fPIC \
    -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
    -DAPTH_CUR_USING_KEYWORD \
    -DAPTH_HOLD_INITIALIZER_PTHREAD \
    -DAPTH_PREEMPT_INSTRUMENT
```

Then rebuild:
```shell
make clean && make all
```

**Step 2: Compile user code with `-finstrument-functions`**

```shell
gcc -finstrument-functions -O2 -o myapp myapp.c -lapth -pthread -ldl
```

Or if using `LD_PRELOAD`:
```shell
gcc -finstrument-functions -O2 -o myapp myapp.c -pthread
LD_PRELOAD=/path/to/libapth.so ./myapp
```

**Step 3 (optional): Tune the threshold**

The default threshold of 10000 function entries works well for most
workloads. Lower values yield more frequently (better fairness, higher
overhead); higher values yield less frequently (lower overhead, less fair).

To change at compile time, add to the library's `CFLAGS`:
```makefile
    -DAPTH_PREEMPT_INSTRUMENT_THRESHOLD=5000
```

**Important notes:**

- Only user code needs `-finstrument-functions`. LIBAPTH's own sources
  are compiled without it (the library's hook functions are marked
  `__attribute__((no_instrument_function))` to avoid infinite recursion).
- The `__cyg_profile_func_enter` and `__cyg_profile_func_exit` symbols
  provided by LIBAPTH are weak. If your application provides its own
  instrumentation hooks (e.g., for profiling), you will need to call
  `apth_preempt_check()` manually from your hook, or use
  `APTH_PREEMPT_SIGNAL` mode instead.
- `-finstrument-functions` adds overhead to every function call. For
  hot inner loops, you can selectively disable it on specific functions:
  ```c
  __attribute__((no_instrument_function))
  void hot_inner_loop(void) { ... }
  ```
- Third-party libraries linked into your application will **not** have
  instrumentation unless they were also compiled with
  `-finstrument-functions`. Long-running computations inside
  uninstrumented library code will not be preempted. If this is a
  concern, use `APTH_PREEMPT_SIGNAL` mode instead.

### Reactor / I/O Flags

| Flag | Default | Description |
|------|---------|-------------|
| `APTH_NUMA` | *Undefined* | When defined, creates one reactor per NUMA node instead of a single global reactor. Stub infrastructure — not yet implemented. |

### Debug Flags

**NOTE**: For developers only.

| Flag | Description |
|------|-------------|
| `APTH_DEBUG` | Enable debug logging throughout the library. |
| `APTH_DEBUG_LLL` | Enable low-level lock (LLL) debug tracing. Requires `APTH_DEBUG`. |
| `APTH_DEBUG_LLL_USING_FPRINTF` | LLL debug output uses `fprintf` instead of the default debug mechanism. |

## Bad APIs
Since `libapth` provides improved performance mainly by changing how workloads
are scheduled, so POSIX thread APIs related to scheduling policy, parameters
and resource competition are actually not functional. Such APIs are listed here:
+ `apth_attr_getscope`, `apth_attr_setscope`
+ `apth_attr_getschedpolicy`, `apth_attr_setschedpolicy`
+ `apth_attr_getschedparam`, `apth_attr_setschedparam`
They are here just to make your code compiles. Besides, `libapth` will provide
an extension for programmer to configure the scheduler.

## Recent Changes

### Performance Overhaul (Phase 4)

#### Assembly Context Switch
Replaced glibc `swapcontext`/`makecontext` with hand-written x86-64 assembly
(`apth_ctx_x86_64.S`).  The old `swapcontext` made two `sigprocmask` syscalls
(kernel round-trips) on every thread switch.  The new assembly saves/restores
only callee-saved registers on the stack:

- **~100x faster context switch**: ~20ns vs ~2000ns
- **Context shrunk from ~940B to 16B**: `ucontext_t` eliminated from TCB
- **No signal mask manipulation**: Signals managed entirely in software via
  per-thread `sigmask` field

#### FD Table Race Fix
The dynamic FD table growth previously had a use-after-free race where
concurrent readers could access freed memory.  Now uses a snapshot-based
approach with deferred free (simplified RCU): old table snapshots are kept
alive until the next grow operation.

#### Event Manager Optimization
`select()` syscalls are no longer called while holding the waiting queue
lock.  SELECT events are collected into a batch during the locked scan and
executed after the lock is released.  This reduces lock hold time from
O(N * syscall) to O(N * pointer-copy).

#### Spinlock Improvement
Scheduler-context spinlocks now use `__builtin_ia32_pause()` spin loops
(no syscall) instead of `sched_yield()` which surrendered the entire CPU
core.

#### Stack Allocation
- Uses `MADV_DONTNEED` instead of `munmap` for recycled stacks, avoiding
  TLB shootdowns.  Guard page protection is preserved across `MADV_DONTNEED`.
- Pool accepts size-compatible stacks (best-fit) instead of exact-match only.
- Pool increased from 32 to 64 entries per scheduler.

#### io_uring Improvements
- Ring depth increased from 256 to 1024 SQEs per scheduler.
- Uses `IORING_SETUP_COOP_TASKRUN` (5.19+) and `IORING_SETUP_SINGLE_ISSUER`
  (6.0+) flags for reduced kernel overhead on per-scheduler rings.
- Graceful fallback on older kernels.

#### Dead Code Removal
- Removed unused scheduler load calculation (`loadval`, `apth_sched_calc_load`)
- Removed unused ring buffer data structure (`ring.c`, `ring.h`)
- Removed dead `apth_ctx_save`/`apth_ctx_restore` functions

### Dynamic FD Table
The global FD table is dynamically allocated and grows as needed (initial
capacity 1024, doubled on demand). This removes the hard `FD_SETSIZE` limit
that previously capped the number of managed file descriptors.

### Earlier Fixes
- **Lock ordering**: Address-based total ordering on all double-lock sites
  (`transfer_one_th`, `transfer_th`, EXIT handler) to prevent deadlocks.
- **Condition variable clock_id**: `apth_cond_init` now stores the
  `clock_id` from attributes. `apth_cond_timedwait` correctly interprets
  `CLOCK_MONOTONIC` deadlines by converting to the internal
  `CLOCK_REALTIME` representation.
- **POLLPRI support**: The `poll()` hook now maps `POLLPRI` to
  `APTH_GOAL_UNTIL_FD_EXCEPTION`.
- **Process exit refcount**: When the alive thread count drops to zero,
  worker 0 is woken immediately for prompt process termination.
- **apth_detach fix**: Replaced a `TODO` panic with proper `EINVAL` return
  when another thread is already joining.
- **Buffer overflow checks**: `__read_chk`, `__pread_chk`, `__pread64_chk`
  now validate `nbytes <= buflen` and abort on overflow, matching glibc
  fortify behavior.

## TODO List
1. ~~Hybrid scheduling~~ **DONE** — `APTH_CLASS_DEDICATED` implemented. Dedicated
threads run as 1:1 pthreads bypassing the scheduler. See `docs/hybrid_scheduling.md`.
2. Check return values.
3. Cancellation points (see pthread(7))
4. Can write tests according to manual pages of pthread
5. For any `apth_t` passed in, check its validity first.
6. Check all passed in arguments to API functions (e.g. `th`) is valid.
7. Hook stream I/O (e.g. printf, fprintf...)
8. Better cancellation (now there's too many fields for cancellation)
9. Should consider the type of the apth (e.g. for GC Worker threads in JVM, it
is better to distribute them evenly across all schedulers, accompanying other
mutator threads)
10. Memory allocator designed deliberately.
11. On Linux platform, mechanisms like signalfd, eventfd should also be
considered and hooked.
12. Fix fork/exec support (child processes currently crash due to reactor
thread state).
13. NUMA-aware multi-reactor (`APTH_NUMA`).

## Memory Allocator
Structures that needs allocation:
1. Stack. Should treated specially.
2. `struct __apth_main_args *__margs__= malloc(sizeof(struct __apth_main_args));`. But this is only one.
3. `iattr->cpuset`. `cpu_set_t`.
4. `malloc(sizeof(struct apth_cleanup_st))`. This struct is currently 3 pointers, meaning 3 words = 12 bytes (32 bits platform) or 24 bytes (64 bits platform). It is aligned to 8 bytes! Should be allocated in per-scheduler TLAB.
5. `tiov = (struct iovec *)malloc(tiovcnt)` in scatter_gather I/O. But could it be optimized to stack allocation?
6. `struct apth_epoll_waiter *w = (struct apth_epoll_waiter *)malloc(sizeof(*w));`
 . They should be allocated in per-scheduler TLAB! So the global memory pool should instead maintain a per-pthread memory block!
7. `sched = (apth_sched_t)malloc(sizeof(struct apth_sched_st))`. This is allocated only once per pthread. So could be allocated in global pool.
8. `t = (apth_t)malloc(sizeof(struct apth_st))`. TCB is now much smaller since
`ucontext_t` (~936B) was replaced with a 16B assembly context.  Could be merged
with the stack allocation to eliminate a separate malloc per thread.
10. `(apth_worker_arg_t)malloc(sizeof(struct apth_worker_pthread_arg))`. Is allocated only once per pthread.
11. `(rv = (char *)malloc(n + 1))`. This is used in `apth_string`. Could we consider allocate on stack? Since LIBAPTH currently do not support time slicing, so we could just do that on scheduler's stack?
