# LIBAPTH Code Review Summary — 2026-03-27

## Reviewers

- **Claude Code (Opus 4.6)**: Comprehensive codebase review — architecture,
  sync primitives, I/O hooks, scheduler, JVM integration APIs, tests.
- **Codex (GPT 5.4)**: Static analysis review — RDMA subsystem, reactor,
  IO_BOUND scheduling semantics.  Follow-up review of Claude's fixes
  identified 3 additional issues (all addressed in second commit).

## Scope

Full review of the libapth codebase to assess production readiness for JVM
disaggregated-memory integration.  96 test files, ~15K lines of core code,
33 I/O hooks, 11 JVM integration APIs examined.

---

## Fixes Implemented in This Commit

### P0 — RDMA subsystem (core use case: latency hiding)

| # | Issue | Fix | Files |
|---|-------|-----|-------|
| 1 | **RDMA poller never started.** `apth_rdma_poller_start()` was declared and implemented but never called from any code path. The slow-path of `apth_rdma_wait()` yielded to the scheduler with no poller thread to detect completions, causing threads to hang forever. (Codex finding) | Added `pthread_once`-based auto-start in `apth_rdma_register_cq()`. The poller thread starts on first CQ registration. Added `apth_rdma_poller_stop()` call to `apth_drop()` for clean shutdown. | `apth_rdma.c`, `apth_init.c` |
| 2 | **Fast-path CQE data loss.** `apth_rdma_wait()` fast path called `ibv_poll_cq()` which destructively removes CQEs from the hardware CQ. If the polled CQE's `wr_id` didn't match, it was silently discarded — permanently losing the completion for another thread sharing the same CQ. (Codex finding) | Non-matching CQEs are now forwarded to `poller_match_completions()` which either delivers them to a matching waiter or stashes them in a new CQE stash buffer. Before the fast-path poll, the stash is checked first. | `apth_rdma.c`, `apth_rdma.h` |
| 3 | **RDMA poller matched on `wr_id` only, not `(cq, wr_id)`.** The waiter struct stored the CQ pointer, but matching ignored it. With multiple CQs (normal for multi-node disaggregated memory), reused WR IDs across CQs could wake the wrong thread. (Codex finding) | `poller_match_completions()` now receives the source CQ and checks `w->cq == source_cq` before matching `wr_id`. | `apth_rdma.c` |
| 4 | **`jvm_integration_guide.md` claimed auto-start that didn't exist.** Documentation said "poller is started automatically when the first CQ is registered" but the code did no such thing. | Fixed documentation to match new auto-start behavior. | `jvm_integration_guide.md` |

### P1 — Correctness bugs

| # | Issue | Fix | Files |
|---|-------|-----|-------|
| 5 | **Worker pool init leak.** If `apth_global_scheduler_pool_init()` failed partway through spawning workers, all previously allocated worker structs and the pointer array leaked. The TODO at line 193 acknowledged this. (Claude finding) | Added `init_fail_cleanup` label that shuts down already-started workers and frees all memory. | `apth_worker.c` |
| 6 | **Worker argument memory leak.** `struct apth_worker_pthread_arg` was malloced per worker but never freed. The TODO at line 131 acknowledged this. (Claude finding) | `scheduler_routine()` now frees the arg immediately after extracting the worker pointer. Removed the TODO comment. | `apth_sched.c`, `apth_worker.c` |
| 7 | **Reactor UNWATCH/FD_CLOSE silently dropped on queue full.** When the reactor request queue (4096 entries) was full, `submit_unwatch()` and `notify_fd_closed()` returned without queuing. Dropped UNWATCH leaves stale waiter state; dropped FD_CLOSE means closed FDs still have active epoll registrations. (Claude + Codex finding) | Both functions now spin-retry when the queue is full, with `reactor_wake()` + `sched_yield()` between retries to give the reactor time to drain. WATCH failures still return -1 (callers can handle). | `apth_reactor.c` |

### P2 — Semantic / behavioral bugs

| # | Issue | Fix | Files |
|---|-------|-----|-------|
| 8 | **IO_BOUND priority lost on Phase 2 FD wake path.** Phase 1 (event manager waiting-queue scan) correctly used `transfer_th_front()` for `APTH_CLASS_IO_BOUND` threads, but the Phase 2 per-scheduler epoll/iouring wake path always used `transfer_th()` — back of queue. In the default `APTH_USE_IOURING` build, this is the hot data-ready path. (Codex finding) | Applied the same class check: IO_BOUND threads use `transfer_th_front()`, others use `transfer_th()`. | `apth_event.c` |

### P3 — Code quality / hardening

| # | Issue | Fix | Files |
|---|-------|-----|-------|
| 9 | **Guard page `mprotect` failure silently ignored.** Failed `mprotect` meant no stack overflow protection, logged as warning and continued. (Claude finding) | `mprotect` failure now frees the stack and TCB, returns `APTH_NULL`. | `apth_tcb.c` |
| 10 | **No sanitizer support in build.** No way to build with AddressSanitizer, ThreadSanitizer, or UBSan. (Claude finding) | Added optional `SANITIZE` variable: `make SANITIZE=address`. | `Makefile` |
| 11 | **Dead commented-out code.** ~30 lines of commented-out worker TLS code in `apth_worker.c`. | Removed. | `apth_worker.c` |

---

## Follow-up Fixes (Second Commit — Codex Review of Claude's Fixes)

Codex reviewed the first commit and identified 3 issues in the new code:

| # | Issue | Fix |
|---|-------|-----|
| 12 | **`pthread_once` non-recoverable.** Auto-start via `pthread_once` swallowed `apth_rdma_poller_start()` return value; if `pthread_create` failed, the once-guard was consumed and subsequent `register_cq()` calls silently succeeded with no poller. Also, `pthread_once_t` survives `apth_drop()`, breaking re-init. | Replaced with explicit state machine (`STOPPED → STARTING → RUNNING → FAILED`). `register_cq()` uses CAS to race for STARTING state; surfaces startup errors to caller. `apth_rdma_poller_stop()` resets to STOPPED for clean re-init. |
| 13 | **CQE stash bounded loss at 256.** Stash overflow silently dropped CQEs, turning "unbounded loss" into "bounded loss" rather than eliminating it. | Stash overflow now logs a warning and increments `stash_overflows` counter. Silent loss → visible error. Fixed ceiling kept (dynamic alloc in hot path undesirable). |
| 14 | **Worker init cleanup NULL deref.** `apth_worker_drop()` dereferences `worker->sched->opening` before the worker pthread sets `sched`. With `malloc` (no zeroing), `sched` was garbage. | Changed `malloc` → `calloc` (zero-init). Cleanup path now spins on `sched == NULL` before calling `apth_worker_drop()`. |

Codex also flagged unsynchronized reads of `cq_count` and `stash_count`
outside their protecting locks. Both are now atomic (`__atomic_load_n` /
`__atomic_store_n`) for TSan cleanliness.

Codex confirmed the IO_BOUND wake fix and reactor queue-full retry are solid.

### Third Commit — Codex Follow-up Review #2

Codex reviewed the second commit and identified 1 remaining issue plus 2
design improvements:

| # | Issue | Fix |
|---|-------|-----|
| 15 | **Worker cleanup hangs if `scheduler_routine` early-returns.** The `while (w->sched == NULL)` spin hangs forever if `scheduler_routine` hits an early-return error (malloc fail, scheduler_init fail) before setting `worker->sched`. | Added `_Atomic(enum apth_worker_state) state` to `struct apth_worker_st` (`STARTING → READY / FAILED`). `scheduler_routine` publishes READY or FAILED. Cleanup path waits for non-STARTING, then calls `apth_worker_drop()` for READY or `pthread_join + free` for FAILED. |
| 16 | **Stash overflow only logged, not surfaced to callers.** Debug log + counter is invisible to callers. | Added per-CQ `cq_faulted[]` flag. Stash overflow marks the source CQ faulted. `apth_rdma_wait*()` fail-fast with `-1`/`EOVERFLOW` on faulted CQs. |
| 17 | **Poller reads CQ list lock-free while unregister writes under lock.** | Poller now snapshots CQ list under `cq_lock` before polling. |

---

## Known Issues NOT Fixed in This Commit

### Should fix (deferred — require deeper design work)

| # | Issue | Severity | Notes |
|---|-------|----------|-------|
| A | **`advised_next_th` use-after-free window** (`apth_sched.c:680-692`). Between atomic_exchange and APTH_IS_VALID check, the thread could be freed. Magic number mitigates but doesn't eliminate. | Medium | Needs generation counter or refcounting on apth_t. |
| B | **Join/detach race condition** (`apth_join.c:30`). If `apth_detach()` is called while another thread is in `apth_join()`, behavior is undefined. Acknowledged TODO. | Medium | Needs state machine redesign for join/detach lifecycle. |
| C | **Dedicated thread `wake_fd` double-close** (`apth_dedicated.c:65` vs `apth_init.c:306`). Multiple code paths can close the same fd. | Low | Needs atomic fd swap or single-owner pattern. |
| D | **State callback not fired on all transitions**. Some queue transfers (NEW→READY) don't fire `__apth_fire_state_callback`. JVM GC may miss notifications. | Medium | Needs audit of all state transition call sites. |
| E | **No tests for `apth_set_state_callback` / `apth_set_preempt_hook`**. Implemented and wired but zero test coverage. | Medium | Add dedicated tests. |
| F | **No graceful `apth_drop()` test under load**. `test_library_init.c` calls `_exit(0)`. | Low | Add test that creates threads, then calls `apth_drop()`. |
| G | **Partial write spin without yield** (I/O hooks). When a write returns partial, the retry loop doesn't yield on EAGAIN between retries. | Low | Insert `apth_yield()` in the partial-write retry path. |
| H | **Signal handlers run on scheduler stack** (`apth_signal.c:116`). Deep signal handler stack usage corrupts scheduler state. Plan B (trampoline + altstack) not yet implemented. | Low | Signal handlers in JVM context should be minimal. |
| I | **Missing `sendmsg`/`recvmsg` hooks**. Used by JVM for complex socket operations. | Low | Add hooks following existing send/recv pattern. |
| J | **RDMA test (`test_rdma_sim.c`) doesn't exercise actual `apth_rdma_wait()`**. Uses `apth_yield()` instead. Mock CQ infrastructure prepared but not connected. (Codex finding) | Medium | Needs mock `ibv_poll_cq` integration or hardware test. |

### Non-issues (investigated and dismissed)

| Claim | Resolution |
|-------|-----------|
| Signal table race (`APTH_GLOBAL_SIGACTIONS`) | **Not a bug.** All user-facing hooks (`signal`, `sigaction`, `__sysv_signal` in `hook_signal.c`) properly acquire `APTH_GLOBAL_SIGACTIONS.lock` before writing. Init/shutdown writes are single-threaded. |
| `thqueue_st.size` not atomic | **Not a bug.** `size` is declared as `_Atomic(size_t)` in `struct_apth_thqueue_st.h:15`. C11 `++`/`--` on `_Atomic` types are atomic operations. `thqueue_size()` reads with `atomic_load_relaxed` for speculative checks. |
| Lost wakeups during safepoint pause | **Not a bug.** `apth_request_pause_all()` already calls `apth_sched_wake()` on all schedulers (writes to `wake_eventfd`), which interrupts `epoll_wait`. The spin-wait loop then waits for `cur == NULL` on all schedulers. |

---

## Architecture Assessment

### What's working well

- **Context switch**: 16-byte assembly context, ~20ns — 100x faster than glibc `swapcontext`.
- **Lock ordering**: Address-based total ordering on all double-lock sites prevents deadlocks.
- **errno preservation**: `apth_shield` macro applied across all 33 I/O hooks.
- **Stack pool**: `MADV_DONTNEED` reuse avoids TLB shootdowns while preserving guard pages.
- **FD table**: Deferred-free RCU pattern for safe concurrent growth.
- **Work stealing**: Two-pass NUMA-aware victim selection with speculative lock-free checks.
- **Hybrid scheduling**: Full `APTH_CLASS_DEDICATED` 1:1 pthread mode, 10 tests.
- **JVM integration**: All 11 APIs implemented, library-mode init, safepoint cooperation.

### What needs architectural attention

- **RDMA path** was the weakest subsystem. Now fixed (poller wiring, CQE handling, CQ matching), but test coverage via mock or real hardware is still needed.
- **Signal delivery** (Plan A: scheduler stack) is adequate for JVM use but fragile for general-purpose applications with deep signal handlers.
- **Thread lifecycle** (join/detach) has acknowledged race conditions that should be resolved before production.

### Test coverage

- **96 test files** covering core threading, sync primitives, I/O hooks, hybrid scheduling, JVM APIs.
- **Pre-existing failure**: `test_fork_wait_apth` (child segfaults, known issue: reactor thread state not fork-safe).
- **Gaps**: State/preempt callbacks, graceful shutdown under load, RDMA end-to-end.

---

## Recommendation

The library is **ready for JVM integration prototyping**. The P0 RDMA fixes
in this commit unblock the core latency-hiding use case. The remaining known
issues (A-J above) are medium/low severity and can be addressed incrementally.

Priority for next iteration:
1. Add RDMA end-to-end test (mock `ibv_poll_cq` or loopback RDMA).
2. Add state/preempt callback tests.
3. Address join/detach race (item B).
4. Address `advised_next_th` UAF (item A) via generation counter.
