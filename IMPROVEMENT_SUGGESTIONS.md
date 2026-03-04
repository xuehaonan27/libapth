# libapth Improvement Suggestions

After a thorough review of the entire codebase, here are categorized suggestions
ranging from critical correctness fixes to architectural improvements.

---

## 1. Critical Correctness Issues

### 1.1 `pwrite` does not advance the file offset on partial writes

**File:** `src/internal/apth_syscall.c:1138-1171`

When `pwrite` does a partial write, `buf` and `count` are advanced, but `offset`
is not. Unlike `write`, `pwrite` writes at an explicit offset, so after a partial
write of `s` bytes, the next `pwrite` call must use `offset + s`:

```c
if (s > 0 && s < (ssize_t)count)
{
    count -= s;
    buf = (void *)((char *)buf + s);
    offset += s;  // <-- missing
    continue;
}
```

### 1.2 `getenv`/`setenv`/`unsetenv` hooks call `TODO()` and crash

**File:** `src/internal/apth_syscall.c:1614-1633`

These hooks are registered via `LD_PRELOAD` but call `TODO(...)` which invokes
`apth_panic_fn` and aborts. Since glibc and many libraries call `getenv`
internally, this can crash at any time. The fix is trivial - just pass through
to the raw libc functions:

```c
APTH_DEFINE_HOOK(char *, getenv, (const char *n), (n))
{
    return apth_func_raw(getenv)(n);
}
```

Same for `setenv` and `unsetenv`.

### 1.3 `fork` hook calls `TODO()` and crashes

**File:** `src/internal/apth_syscall.c:467-470`

Same issue as above. At minimum, `fork` should fall through to the raw syscall
and handle the child process appropriately (e.g., reset scheduler state in the
child). Note that the `system()` hook calls `apth_syscall(fork)()` which
will currently crash.

### 1.4 `apth_join` CAS logic appears inverted

**File:** `src/core/apth_join.c:45-47`

```c
else if (apth_unlikely(atomic_compare_exchange_weak_acquire(&tid->joinid, &self, NULL)))
    return apth_error(EINVAL, EINVAL);
```

This CAS checks if `tid->joinid == self` and tries to swap it to NULL. But the
intent is to check if someone *else* is already joining (i.e., `tid->joinid` is
non-NULL and not `self`) and, if no one is, set `tid->joinid = self` to claim it.
The correct logic should be:

```c
apth_t expected = NULL;
if (!atomic_compare_exchange_weak_acquire(&tid->joinid, &expected, self))
    // Someone else is already joining
    return apth_error(EINVAL, EINVAL);
```

### 1.5 `apth_fd_acquire` race on `orig_flags`

**File:** `src/internal/apth_fd.c:41-67`

When two threads concurrently call `apth_fd_acquire` on an unmanaged fd, both
may succeed the CAS on `e->managed` (one wins, the other sees `managed==1` in
the fast path). But the loser thread may read `e->orig_flags` before the winner
has stored it (line 51), resulting in stale/zero flags. Consider protecting the
first-time setup with a lock or using a combined atomic state.

### 1.6 Wake batch overflow silently drops apths

**File:** `src/internal/apth_event.c:697-871`

`MAX_WAKE_BATCH` is 128. If more than 128 apths need waking in one event manager
pass, the extras are silently dropped until the next iteration. This causes
latency spikes under high concurrency. Fix by looping until all are processed,
or by dynamically sizing the batch.

---

## 2. Performance Improvements

### 2.1 `ucontext` context switch triggers a syscall on every swap (highest impact)

**Files:** `src/internal/apth_ctx.c`

`swapcontext()` / `getcontext()` / `setcontext()` internally call `sigprocmask`
on every invocation, which is a kernel syscall. This is the **single biggest
performance bottleneck** in the library, as the scheduler does two context
switches per scheduled apth (sched->apth and apth->sched).

**Recommendation:** Replace `ucontext` with a hand-rolled context switch using
inline assembly (save/restore only callee-saved registers + stack pointer +
instruction pointer). This is what Go's goroutine scheduler, Boost.Context, and
libaco all do. On x86-64, this is ~10 instructions vs the ~200 instruction +
syscall path of `swapcontext`. Expected speedup: 10-50x for context switch
overhead.

A minimal x86-64 implementation:

```asm
// apth_swap_context(old_sp_ptr, new_sp)
apth_swap_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, (%rdi)    // save old SP
    movq  %rsi, %rsp      // load new SP
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    ret
```

### 2.2 Event and waiter allocations on the hot path

**Files:** `src/internal/apth_event.c:1163-1178`, `src/internal/apth_event.c:25`

Every I/O wait does: `malloc(event)` -> use -> `free(event)`. The epoll waiter
map does the same with `struct apth_epoll_waiter`. On a high-throughput server,
this can be millions of malloc/free calls per second.

**Recommendation:** Use a per-scheduler free-list (slab allocator) for both
`apth_event_st` and `apth_epoll_waiter`. Pre-allocate a pool of e.g., 256
entries and recycle them. Fallback to malloc only when the pool is exhausted.

### 2.3 Scheduler busy-spins when idle

**File:** `src/internal/apth_sched.c:319-327`

When no apth is ready and the event manager finds nothing, the scheduler calls
`sched_yield()` in a tight loop, burning CPU. This defeats the purpose of the
library (improving CPU utilization).

**Recommendation:** When the scheduler has truly nothing to do (no ready, no new,
no waiting), it should block on `epoll_wait` with a reasonable timeout (e.g.,
100ms) or use a `futex`/`eventfd` to be woken when new work arrives. The
`apth_create` path should wake the target scheduler when pushing to `new_queue`.

### 2.4 `thqueue_size` takes a lock for a simple counter read

**File:** `src/internal/apth_thqueue.c:37-45`

`thqueue_size` is called multiple times per scheduler loop iteration to check
queue emptiness. Each call acquires and releases a lock.

**Recommendation:** Make `size` an `_Atomic(size_t)` and use `atomic_load_acquire`
for reads. Updates (already under lock) should use `atomic_store_release`. This
eliminates lock overhead for the very common "is queue empty?" check.

### 2.5 `gettimeofday` called excessively

**File:** `src/internal/apth_sched.c:275,323,338,359`

`APTH_TIME_NOW` calls `gettimeofday` every time. The scheduler main loop calls
it 4+ times per iteration.

**Recommendation:**
- Use `clock_gettime(CLOCK_MONOTONIC_COARSE)` which is vDSO-backed and much
  cheaper (no actual syscall). Also, `CLOCK_MONOTONIC` is better than
  `gettimeofday` because it doesn't jump with NTP adjustments.
- Cache the timestamp at the top of each scheduler iteration and reuse it.

### 2.6 `fd_slot_table` is statically sized at `FD_SETSIZE` per scheduler

**File:** `src/internal_types.h:218`

Each `apth_perpthr_scheduler` embeds 1024 `struct apth_epoll_fd_slot` entries
(~40+ bytes each), totaling ~40KB+ per scheduler regardless of actual FD usage.
With 8 workers, that's 320KB wasted.

**Recommendation:** Use a hash map or dynamically sized array. Alternatively,
since epoll already uses `data.fd`, you can look up slots on demand with a
much smaller table sized to actual usage.

---

## 3. Architecture Improvements

### 3.1 Add cooperative preemption via timer signals

Currently, a CPU-bound apth that never calls `apth_yield()` or performs I/O
will starve all other apths on its scheduler indefinitely. This is the most
user-visible limitation of the library.

**Recommendation:** Use `timer_create` with `SIGALRM`/`SIGPROF` to deliver a
periodic signal to each worker pthread. The signal handler saves the current
apth's context and switches back to the scheduler, implementing time-slicing.
This is how Go's goroutine scheduler works (with `SIGURG`).

### 3.2 Implement work stealing

**Files:** `src/internal/apth_sched.c:322` (TODO comment)

When one scheduler is idle while another is overloaded, there's no rebalancing.
This leads to poor CPU utilization with uneven workloads.

**Recommendation:** Allow idle schedulers to steal from the **back** of another
scheduler's ready queue (to minimize contention with the victim's front-of-queue
dispatch). The `thqueue` locking already supports cross-scheduler access. A
simple round-robin scan of other schedulers' ready queues when idle would be a
good starting point.

### 3.3 Replace waiting queue linear scan with efficient data structures

**File:** `src/internal/apth_event.c:672-1022`

The event manager traverses ALL waiting apths and ALL their events in O(n*m)
every iteration. This becomes a bottleneck with many waiting apths.

**Recommendation:**
- **Timer events:** Use a min-heap (priority queue) keyed by expiration time.
  The scheduler only needs to check the root to see if the nearest timer has
  expired. Cost: O(log n) insert/remove vs O(n) scan.
- **FD events:** Already indexed by fd in `fd_slot_table`, which is good.
  However, the phase-1 linear scan of the waiting queue to register FD events
  is redundant - register them at the time the apth enters the waiting state.
- **TID events:** Maintain a per-apth "waiters" list so that when an apth
  terminates, it can directly wake its waiters without scanning.

### 3.4 Remove dead `apth_sched_eventmanager` (select-based)

**File:** `src/internal/apth_event.c:1026-1161`

The old select-based event manager is no longer called from the scheduler (which
uses `apth_sched_eventmanager_epoll`). It adds 130+ lines of dead code and
maintenance burden. Remove it or gate it behind a `#ifdef` for portability to
non-Linux platforms.

### 3.5 Implement `apth_mutex` and `apth_cond`

**Files:** `src/internal_types.h:429-438`

Mutexes and condition variables are fundamental synchronization primitives.
Without them, users must either use the low-level `lll_t` (not public API) or
fall back to pthread mutexes (which defeats the purpose of userspace threading).

An `apth_mutex` should:
- Fast-path: CAS to acquire, no syscall needed.
- Slow-path: Add the apth to the mutex's wait list, submit
  `APTH_STATE_WAITING`, yield to scheduler. The event manager (or the unlocking
  apth) wakes the next waiter.

### 3.6 Hook `fcntl` to prevent user code from conflicting with non-blocking mode

**File:** `src/internal/apth_syscall.c:1608-1612` (commented out)

libapth temporarily sets FDs to `O_NONBLOCK`. If user code calls
`fcntl(fd, F_SETFL, ...)` and removes `O_NONBLOCK` while another apth is in a
non-blocking I/O loop on the same FD, the I/O will block the entire scheduler.

**Recommendation:** Hook `fcntl`. For `F_SETFL`, record the user's desired flags
but keep `O_NONBLOCK` active if `refcount > 0`. Restore the user's flags only
when refcount drops to 0.

---

## 4. Code Quality and Robustness

### 4.1 `_GNU_SOURCE` / `_POSIX_C_SOURCE` defined in headers after includes

**File:** `src/internal_types.h:3,13`

```c
#include "apth.h"       // includes happen before _POSIX_C_SOURCE
...
#define _POSIX_C_SOURCE 200809L  // too late, apth.h already included headers
```

These feature test macros must be defined **before any system header inclusion**
to have effect. Move them to compiler flags in the Makefile:

```makefile
CFLAGS := ... -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
```

### 4.2 Remove dead code: `transfer_thqueue`

**File:** `src/internal/apth_thqueue.c:206-237`

The entire function body is commented out. Either implement it or remove it.

### 4.3 Memory leak on init failure

**File:** `src/internal/apth_worker.c:270-285`

If `apth_worker_init` fails for worker N, workers 0..N-1 are leaked (as noted
by the TODO). Add a cleanup loop on failure.

### 4.4 Stack guard should use `mprotect` for real protection

**File:** `src/internal/apth_tcb.c:65-72`

The current guard is a single `uint32_t` magic number at the stack boundary.
A stack overflow that happens to skip this word (e.g., a large stack frame) goes
undetected until it corrupts another apth's memory.

**Recommendation:** Allocate stack memory with `mmap` (for page alignment) and
call `mprotect(guard_page, PAGE_SIZE, PROT_NONE)` on the guard region. This
makes any stack overflow immediately trigger `SIGSEGV` from hardware, which is
both faster (no software check needed) and more reliable.

### 4.5 `__read_chk` / `__pread_chk` / `__recv_chk` / `__recvfrom_chk` don't check

**Files:** `src/internal/apth_syscall.c:994-1001, 1110-1117, 1427-1435, 1515-1522`

These `_chk` variants are fortified-source wrappers that glibc calls when
`-D_FORTIFY_SOURCE` is enabled. They receive `buflen` (the actual buffer size)
but don't validate `nbytes <= buflen`. If `nbytes > buflen`, the library should
call `__chk_fail()` to abort, matching glibc's behavior:

```c
if (nbytes > buflen)
    __chk_fail();
```

### 4.6 `apth_error` return value inconsistency

Throughout the codebase, `apth_error` is used with different return value
conventions:
- Some callers: `return apth_error(-1, EINVAL)` (returns -1)
- Some callers: `return apth_error(EINVAL, EINVAL)` (returns errno value)
- Some callers: `return apth_error(false, ENOMEM)` (returns bool)

This makes it hard to reason about error paths. Consider standardizing: functions
that return `int` error codes should use `-1` on failure with `errno` set.
Functions that return `0`/error-code (like pthread conventions) should return
the error code directly without setting `errno`.

---

## 5. Testing and Build

### 5.1 Add a CI test for `LD_PRELOAD` compatibility

The library's primary use case is via `LD_PRELOAD`. Add automated tests that:
- Run a standard program (e.g., `curl`, `ls`, `python3 -c "print('hello')"`)
  with `LD_PRELOAD=libapth.so` to verify it doesn't crash.
- This catches issues like the `getenv` crash described in 1.2.

### 5.2 Add stress tests for high apth counts

Current tests appear to focus on correctness with small numbers of apths. Add
tests that spawn 1000+ apths doing concurrent I/O to stress-test the scheduler,
event manager, and memory allocation paths.

### 5.3 Dependency tracking in Makefile

**File:** `Makefile:108`

The object file rule doesn't generate dependency files (`.d` files) as part of
compilation. The `-include` at line 313 relies on a separate rule to generate
them. Use `-MMD -MP` flags in the compile step to auto-generate dependencies:

```makefile
CFLAGS := ... -MMD -MP
```

---

## Priority Ranking

| Priority | Item | Impact |
|----------|------|--------|
| **P0** | 1.2 getenv/setenv/unsetenv crash | Any LD_PRELOAD usage likely crashes |
| **P0** | 1.3 fork crash | system() calls crash |
| **P0** | 1.1 pwrite offset bug | Data corruption |
| **P0** | 1.4 apth_join CAS logic | Incorrect join behavior |
| **P1** | 2.1 Replace ucontext | 10-50x context switch speedup |
| **P1** | 3.1 Timer preemption | Prevents starvation |
| **P1** | 2.3 Stop busy-spinning | Eliminates wasted CPU |
| **P1** | 1.5 fd_acquire race | Data corruption under concurrency |
| **P2** | 2.2 Slab allocator for events | Reduces malloc overhead |
| **P2** | 3.2 Work stealing | Better load balancing |
| **P2** | 3.3 Efficient event data structures | O(log n) vs O(n) event checking |
| **P2** | 3.5 Mutex/condvar | Fundamental missing feature |
| **P2** | 2.4 Lockfree queue size | Reduces lock contention |
| **P3** | 2.5 Monotonic clock | Minor syscall reduction |
| **P3** | 3.4 Remove dead code | Maintenance |
| **P3** | 4.1 Feature macros in CFLAGS | Correctness of feature detection |
| **P3** | 4.4 mprotect stack guard | Better overflow detection |
| **P3** | 3.6 Hook fcntl | Prevent FD mode conflicts |
