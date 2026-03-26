# Hybrid Scheduling: APTH_CLASS_DEDICATED

## Overview

LIBAPTH now supports **hybrid scheduling** via the `APTH_CLASS_DEDICATED` thread
class. A dedicated thread gets its own 1:1 pthread, bypassing the M:N userspace
scheduler entirely. This enables scenarios where some threads need full kernel
thread semantics (CPU-bound computation, JVM JIT compiler, signal handling) while
coexisting with cooperatively scheduled apths that benefit from fast userspace
context switches.

## Architecture

```
Regular apths (M:N cooperative):        Dedicated threads (1:1 pthread):
┌──────────────────────────┐            ┌─────────────────────────┐
│ Scheduler Worker 0       │            │ Dedicated pthread 0     │
│ (pthread, CPU-pinned)    │            │ (own pthread, NOT in    │
│ ┌───┐ ┌───┐ ┌───┐       │            │  any scheduler queue)   │
│ │th1│ │th2│ │th3│ ...    │            │ ┌─────────────────┐     │
│ └───┘ └───┘ └───┘       │            │ │ user function    │     │
│ cooperative scheduling    │◄─ sync ─► │ │ (blocking I/O)   │     │
│ I/O hooks active          │           │ └─────────────────┘     │
└──────────────────────────┘            └─────────────────────────┘
```

## API

### Creating a Dedicated Thread

```c
apth_t tid;
apth_attr_t attr;
apth_attr_init(&attr);
apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);

apth_create(&tid, &attr, my_cpu_bound_func, arg);
apth_attr_destroy(&attr);

// ... later ...
void *result;
apth_join(tid, &result);
```

### Key Behaviors

| Behavior | Regular apth (M:N) | Dedicated (1:1) |
|----------|-------------------|-----------------|
| Backing thread | Shares worker pthread | Own private pthread |
| I/O calls | Hooked (nonblock + yield) | Raw blocking syscalls |
| `apth_yield()` | Userspace context switch (~20ns) | Kernel `sched_yield()` |
| Preemption | SIGPROF timer (if enabled) | Blocked (not preempted by LIBAPTH) |
| Work stealing | Can be stolen by other schedulers | Never stolen |
| Sync primitives | Yield to scheduler on contention | Block on eventfd on contention |
| Thread creation from | Assigns to current scheduler | Assigns to scheduler 0 |

### Synchronization Interop

Dedicated threads can share all synchronization primitives with regular apths:

- **Mutex** (`apth_mutex_t`): Dedicated threads block on an eventfd when contended,
  regular apths yield to their scheduler. Unlocking a mutex wakes the appropriate
  thread via the correct mechanism.

- **Condition variables** (`apth_cond_t`): Same pattern. `apth_cond_wait` from a
  dedicated thread blocks on eventfd; `apth_cond_signal`/`apth_cond_broadcast`
  dispatches to the correct wake mechanism per waiter.

- **Barriers**, **semaphores**, **rwlocks**: All support mixed dedicated/regular threads.

### CPU Affinity

Dedicated threads support CPU affinity via `apth_attr_setaffinity_np()`, which is
passed through to the backing `pthread_create`.

### Detached Dedicated Threads

Detached dedicated threads (via `apth_attr_setdetachstate` or `apth_detach`) clean
up their own resources (eventfd, dummy scheduler, TCB) when the thread exits.

## Implementation Details

### TLS Compatibility

Each dedicated thread has a **dummy scheduler** (`id = -1`) allocated for TLS
compatibility. This makes `CUR_APTH`, `CUR_SCHED`, and all LIBAPTH macros work
correctly without special-casing throughout the codebase. The dummy scheduler is
never used for actual scheduling.

### Blocking Mechanism

When a dedicated thread must wait (sync primitive contention, joining another
thread), it blocks on a per-thread `eventfd` (`dedicated_wake_fd`):

- **Block**: `read(dedicated_wake_fd)` — blocks the pthread until a write occurs
- **Unblock**: `write(dedicated_wake_fd, 1)` — safe from any context

This avoids the need for the scheduler's event manager to process dedicated threads.

### I/O Hook Bypass

Every hooked I/O function (72 total across 12 source files) checks
`CUR_APTH->is_dedicated` at entry and immediately forwards to the raw libc
syscall for dedicated threads. This means:

- FDs are NOT set to `O_NONBLOCK`
- FDs are NOT registered with LIBAPTH's FD table
- No scheduler involvement for I/O

### Thread Lifecycle

```
apth_create(APTH_CLASS_DEDICATED):
  → malloc TCB (no mmap stack — uses pthread's stack)
  → create eventfd (blocking mode)
  → alloc dummy scheduler (id=-1)
  → pthread_create(dedicated_thread_wrapper)

dedicated_thread_wrapper:
  → SET_CUR_SCHED(dummy), SET_CUR_APTH(self)
  → Block SIGPROF
  → Set state = RUNNING
  → Call user's start_func
  → apth_dedicated_do_exit:
      → Cleanup handlers + TLS destructors
      → Set state = TERMINATED
      → dec_alive_thrcnt()
      → Wake joiner
  → (detached: free TCB, close eventfd, free dummy scheduler)
  → pthread returns

apth_join(dedicated_tid):
  → Regular apth caller: EVENT_TID wait (yields to scheduler)
  → Dedicated caller: block on own wake_fd
  → After TERMINATED: pthread_join (immediate), free resources
```

## JVM Integration Use Case

For the JVM integration described in the parent CLAUDE.md:

```c
// JIT compiler thread — CPU-bound, doesn't benefit from userspace scheduling
apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
apth_create(&jit_tid, &attr, jit_compile_loop, NULL);

// Mutator threads — I/O-bound (RDMA waits), benefit from M:N scheduling
apth_attr_setclass_np(&attr, APTH_CLASS_IO_BOUND);
apth_create(&mutator_tid, &attr, mutator_func, NULL);

// GC workers — CPU-bound but cooperatively scheduled with mutators
apth_attr_setclass_np(&attr, APTH_CLASS_CPU_BOUND);
apth_create(&gc_tid, &attr, gc_worker_func, NULL);
```

## Files Changed

| Category | Files |
|----------|-------|
| API | `src/apth.h`, `src/attr/apth_attr_setclass_np.c` |
| Data structures | `src/internal/types/struct_apth_st.h` |
| Core lifecycle | `src/core/apth_dedicated.c` (new), `src/core/apth_create.c`, `src/core/apth_yield.c`, `src/core/apth_exit.c`, `src/core/apth_join.c`, `src/core/apth_detach.c` |
| Scheduler | `src/internal/apth_sched.c`, `src/internal/apth_sched.h` |
| TCB | `src/internal/apth_tcb.c` |
| Sync primitives | `src/core/apth_mutex.c`, `src/core/apth_cond.c`, `src/core/apth_barrier.c`, `src/core/apth_sem.c`, `src/core/apth_rwlock.c` |
| I/O hooks | 12 files under `src/hook_libc/` (72 bypass points) |
| Tests | 10 new test files under `test/` |
