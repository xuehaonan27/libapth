#include "apth_rdma.h"

#ifdef APTH_USE_RDMA

#include "common.h"
#include "internal/apth_event.h"
#include "internal/apth_sched.h"
#include "internal/types.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/lll.inline.h"
#include <infiniband/verbs.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>

struct apth_rdma_poller GLOBAL_RDMA_POLLER;

/* Poller lifecycle state machine: STOPPED → STARTING → RUNNING / FAILED.
 * apth_rdma_poller_stop() resets to STOPPED so re-init after apth_drop()
 * works.  Unlike pthread_once, this surfaces startup failures and allows
 * retry. */
enum { RDMA_POLLER_STOPPED = 0, RDMA_POLLER_STARTING, RDMA_POLLER_RUNNING, RDMA_POLLER_FAILED };
static _Atomic(int) rdma_poller_state = RDMA_POLLER_STOPPED;

/* ================================================================
 * Poller main loop
 *
 * Busy-polls all registered CQs.  ibv_poll_cq is a userspace memory
 * read (~10-50ns), not a syscall, so this is extremely efficient.
 * When no completions are found, brief pause instructions avoid
 * burning CPU while keeping latency low.
 * ================================================================ */

/* Wake a waiter: futex for DEDICATED threads, scheduler wake for M:N. */
static inline void waiter_wake(struct apth_rdma_waiter *w)
{
    if (w->futex_ptr)
    {
        /* DEDICATED thread: wake via futex. */
        __atomic_store_n(w->futex_ptr, 1, __ATOMIC_RELEASE);
        syscall(SYS_futex, w->futex_ptr, FUTEX_WAKE_PRIVATE,
                1, NULL, NULL, 0);
    }
    else if (w->sched)
    {
        apth_sched_wake(w->sched);
    }
}

static void poller_match_completions(struct ibv_cq *source_cq,
                                     struct ibv_wc *wcs, int n)
{
    struct apth_rdma_poller *p = &GLOBAL_RDMA_POLLER;

    for (int i = 0; i < n; i++)
    {
        uint64_t wr_id = wcs[i].wr_id;
        bool matched = false;

        /* Find the waiter matching (cq, wr_id).
         * Linear scan is acceptable because:
         * 1. The waiter table is cache-hot (poller accesses it every iteration)
         * 2. Typical active waiter count is small (tens, not thousands)
         * 3. We scan from the back (most recent) for temporal locality */
        lll_internal_lock(&p->waiter_lock);
        int wcount = atomic_load_acquire(&p->waiter_count);
        for (int j = wcount - 1; j >= 0; j--)
        {
            struct apth_rdma_waiter *w = &p->waiters[j];
            if (!w->active)
                continue;
            if (w->cq != source_cq || w->wr_id != wr_id)
                continue;

            /* Match found.  Store the completion and mark event. */
            if (w->wc_out)
                *w->wc_out = wcs[i];
            __atomic_store_n(&w->ev->ev_status, APTH_EV_STATUS_OCCURRED, __ATOMIC_RELEASE);
            w->active = false;

            /* Wake the owning thread. */
            waiter_wake(w);

            matched = true;
            break;
        }

        /* No waiter matched — stash for later retrieval by fast-path callers.
         * ibv_poll_cq destructively removes CQEs from the CQ, so we must
         * preserve unmatched completions to avoid data loss. */
        if (!matched)
        {
            if (p->stash_count < APTH_RDMA_STASH_SIZE)
            {
                struct apth_rdma_stashed_cqe *s = &p->stash[p->stash_count++];
                s->cq = source_cq;
                s->wc = wcs[i];
            }
            else
            {
                /* Stash full — CQE will be lost.  Mark the source CQ as
                 * faulted so subsequent apth_rdma_wait*() on it fails with
                 * EOVERFLOW instead of hanging silently. */
                apth_debug("WARNING: RDMA CQE stash full (%d), dropping "
                           "completion wr_id=%lu on cq=%p — CQ faulted",
                           APTH_RDMA_STASH_SIZE,
                           (unsigned long)wcs[i].wr_id, source_cq);
                __atomic_add_fetch(&p->stash_overflows, 1, __ATOMIC_RELAXED);
                /* Find the CQ slot and mark faulted. */
                for (int ci = 0; ci < __atomic_load_n(&p->cq_count, __ATOMIC_ACQUIRE); ci++)
                {
                    if (p->cqs[ci].cq == source_cq)
                    {
                        __atomic_store_n(&p->cq_faulted[ci], true, __ATOMIC_RELEASE);
                        break;
                    }
                }

                /* Fail and wake ALL active waiters on the faulted CQ.
                 * Without this, threads already in rdma_poller_submit()
                 * slow-path would hang forever if their CQE was the one
                 * that got dropped. We hold waiter_lock here. */
                int wcount = atomic_load_acquire(&p->waiter_count);
                for (int wi = 0; wi < wcount; wi++)
                {
                    struct apth_rdma_waiter *w = &p->waiters[wi];
                    if (!w->active || w->cq != source_cq)
                        continue;
                    __atomic_store_n(&w->ev->ev_status,
                                     APTH_EV_STATUS_FAILED, __ATOMIC_RELEASE);
                    w->active = false;
                    waiter_wake(w);
                }
            }
        }

        lll_internal_unlock(&p->waiter_lock);
    }
}

/* Compact the waiter table by removing inactive entries.
 * Called periodically to keep the table small. */
static void poller_compact_waiters(void)
{
    struct apth_rdma_poller *p = &GLOBAL_RDMA_POLLER;
    lll_internal_lock(&p->waiter_lock);
    int wcount = atomic_load_acquire(&p->waiter_count);
    int dst = 0;
    for (int src = 0; src < wcount; src++)
    {
        if (p->waiters[src].active)
        {
            if (dst != src)
                p->waiters[dst] = p->waiters[src];
            dst++;
        }
    }
    atomic_store_release(&p->waiter_count, dst);
    lll_internal_unlock(&p->waiter_lock);
}

static void *rdma_poller_func(void *arg)
{
    (void)arg;
    struct apth_rdma_poller *p = &GLOBAL_RDMA_POLLER;
    struct ibv_wc wc_batch[64];
    int idle_spins = 0;
    int compact_counter = 0;

    apth_debug("RDMA poller thread started");

    while (__atomic_load_n(&p->running, __ATOMIC_ACQUIRE))
    {
        bool any_work = false;

        /* Snapshot the CQ list under cq_lock so unregister_cq() can
         * safely mark entries inactive without racing the poller. */
        struct apth_rdma_cq_entry cq_snap[APTH_RDMA_MAX_CQS];
        int cq_snap_count;
        lll_internal_lock(&p->cq_lock);
        cq_snap_count = p->cq_count;
        for (int i = 0; i < cq_snap_count; i++)
            cq_snap[i] = p->cqs[i];
        lll_internal_unlock(&p->cq_lock);

        for (int i = 0; i < cq_snap_count; i++)
        {
            if (!cq_snap[i].active)
                continue;

            int n = ibv_poll_cq(cq_snap[i].cq, 64, wc_batch);
            if (n <= 0)
                continue;

            any_work = true;
            poller_match_completions(cq_snap[i].cq, wc_batch, n);
        }

        /* Try to match stashed CQEs against newly registered waiters.
         * Speculative check outside lock (atomic for TSan). */
        if (__atomic_load_n(&p->stash_count, __ATOMIC_ACQUIRE) > 0)
        {
            lll_internal_lock(&p->waiter_lock);
            int wcount = atomic_load_acquire(&p->waiter_count);
            int new_stash = 0;
            for (int si = 0; si < p->stash_count; si++)
            {
                struct apth_rdma_stashed_cqe *s = &p->stash[si];
                bool found = false;
                for (int j = wcount - 1; j >= 0; j--)
                {
                    struct apth_rdma_waiter *w = &p->waiters[j];
                    if (!w->active)
                        continue;
                    if (w->cq != s->cq || w->wr_id != s->wc.wr_id)
                        continue;
                    if (w->wc_out)
                        *w->wc_out = s->wc;
                    __atomic_store_n(&w->ev->ev_status, APTH_EV_STATUS_OCCURRED, __ATOMIC_RELEASE);
                    w->active = false;
                    waiter_wake(w);
                    found = true;
                    any_work = true;
                    break;
                }
                if (!found)
                {
                    /* Keep in stash for next round. */
                    if (new_stash != si)
                        p->stash[new_stash] = p->stash[si];
                    new_stash++;
                }
            }
            p->stash_count = new_stash;
            lll_internal_unlock(&p->waiter_lock);
        }

        if (any_work)
        {
            idle_spins = 0;
            compact_counter++;
            /* Compact waiter table every 256 poll rounds to keep it small. */
            if (compact_counter >= 256)
            {
                poller_compact_waiters();
                compact_counter = 0;
            }
        }
        else
        {
            /* No completions found.  Brief pause to save power. */
            idle_spins++;
            if (idle_spins < 64)
            {
                /* Hot spin: ~40 cycles per pause. */
                for (int i = 0; i < 4; i++)
                    __asm__ volatile("pause" ::: "memory");
            }
            else if (idle_spins < 1024)
            {
                /* Warm spin: yield to kernel scheduler briefly. */
                sched_yield();
            }
            else
            {
                /* Cold: sleep 100μs.  Completions are rare. */
                struct timespec ts = {0, 100000}; /* 100 μs */
                nanosleep(&ts, NULL);
            }
        }
    }

    apth_debug("RDMA poller thread exiting");
    return NULL;
}

/* ================================================================
 * Lifecycle
 * ================================================================ */

APTH_INTERNAL int apth_rdma_poller_start(void)
{
    struct apth_rdma_poller *p = &GLOBAL_RDMA_POLLER;
    memset(p, 0, sizeof(*p));

    lll_internal_init(&p->cq_lock);
    lll_internal_init(&p->waiter_lock);
    atomic_store_release(&p->waiter_count, 0);
    p->cq_count = 0;
    p->stash_count = 0;

    __atomic_store_n(&p->running, true, __ATOMIC_RELEASE);

    int ret = pthread_create(&p->thread, NULL, rdma_poller_func, NULL);
    if (ret != 0)
    {
        __atomic_store_n(&p->running, false, __ATOMIC_RELEASE);
        atomic_store_release(&rdma_poller_state, RDMA_POLLER_FAILED);
        apth_debug("RDMA poller pthread_create failed: %d", ret);
        return -1;
    }

    atomic_store_release(&rdma_poller_state, RDMA_POLLER_RUNNING);
    apth_debug("RDMA poller started");
    return 0;
}

APTH_INTERNAL void apth_rdma_poller_stop(void)
{
    struct apth_rdma_poller *p = &GLOBAL_RDMA_POLLER;
    if (!__atomic_load_n(&p->running, __ATOMIC_ACQUIRE))
    {
        /* Reset state so re-init after apth_drop() can restart. */
        atomic_store_release(&rdma_poller_state, RDMA_POLLER_STOPPED);
        return;
    }

    __atomic_store_n(&p->running, false, __ATOMIC_RELEASE);
    pthread_join(p->thread, NULL);

    /* Reset to STOPPED so the next apth_init_library() + register_cq()
     * cycle can restart the poller cleanly. */
    atomic_store_release(&rdma_poller_state, RDMA_POLLER_STOPPED);
    apth_debug("RDMA poller stopped");
}

/* ================================================================
 * CQ registration
 * ================================================================ */

APTH_API int apth_rdma_register_cq(struct ibv_cq *cq)
{
    /* Auto-start the poller thread on first CQ registration.
     * Uses CAS so only one thread executes start; others spin until
     * the state reaches RUNNING or FAILED. */
    for (;;)
    {
        int state = atomic_load_acquire(&rdma_poller_state);
        if (state == RDMA_POLLER_RUNNING)
            break;
        if (state == RDMA_POLLER_FAILED)
            return -1; /* Previous start failed; caller must handle. */
        if (state == RDMA_POLLER_STOPPED)
        {
            int expected = RDMA_POLLER_STOPPED;
            if (atomic_compare_exchange_acquire(&rdma_poller_state,
                                                 &expected, RDMA_POLLER_STARTING))
            {
                /* We won the race — start the poller. */
                if (apth_rdma_poller_start() != 0)
                    return -1; /* State already set to FAILED inside start. */
                break;
            }
            /* Another thread won; loop and re-check. */
        }
        /* STARTING: another thread is starting; spin briefly. */
        __asm__ volatile("pause" ::: "memory");
    }

    struct apth_rdma_poller *p = &GLOBAL_RDMA_POLLER;

    lll_internal_lock(&p->cq_lock);
    int cur_count = p->cq_count;
    if (cur_count >= APTH_RDMA_MAX_CQS)
    {
        lll_internal_unlock(&p->cq_lock);
        return -1;
    }
    p->cqs[cur_count].cq = cq;
    p->cqs[cur_count].active = true;
    /* Atomic store so the poller's lock-free read sees the new entry only
     * after cq/active are fully written. */
    __atomic_store_n(&p->cq_count, cur_count + 1, __ATOMIC_RELEASE);
    lll_internal_unlock(&p->cq_lock);

    apth_debug("RDMA CQ registered: %p (total=%d)", cq, cur_count + 1);
    return 0;
}

APTH_API void apth_rdma_unregister_cq(struct ibv_cq *cq)
{
    struct apth_rdma_poller *p = &GLOBAL_RDMA_POLLER;

    lll_internal_lock(&p->cq_lock);
    for (int i = 0; i < p->cq_count; i++)
    {
        if (p->cqs[i].cq == cq)
        {
            p->cqs[i].active = false;
            break;
        }
    }
    lll_internal_unlock(&p->cq_lock);
}

/* ================================================================
 * Wait for RDMA completion
 * ================================================================ */

/* Submit a waiter to the poller.  Thread-safe. */
static int rdma_poller_submit(apth_event_t ev, apth_t th, apth_sched_t sched,
                               struct ibv_cq *cq, uint64_t wr_id,
                               struct ibv_wc *wc_out,
                               volatile int *futex_ptr)
{
    struct apth_rdma_poller *p = &GLOBAL_RDMA_POLLER;

    lll_internal_lock(&p->waiter_lock);
    int idx = atomic_load_acquire(&p->waiter_count);
    if (idx >= APTH_RDMA_MAX_WAITERS)
    {
        lll_internal_unlock(&p->waiter_lock);
        return -1;
    }

    struct apth_rdma_waiter *w = &p->waiters[idx];
    w->ev = ev;
    w->th = th;
    w->sched = sched;
    w->cq = cq;
    w->wr_id = wr_id;
    w->wc_out = wc_out;
    w->active = true;
    w->futex_ptr = futex_ptr;
    atomic_store_release(&p->waiter_count, idx + 1);
    lll_internal_unlock(&p->waiter_lock);

    return 0;
}

/* Check if a CQ is in faulted state (stash overflow caused CQE loss). */
static bool rdma_cq_is_faulted(struct apth_rdma_poller *p, struct ibv_cq *cq)
{
    int count = __atomic_load_n(&p->cq_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < count; i++)
    {
        if (p->cqs[i].cq == cq)
            return __atomic_load_n(&p->cq_faulted[i], __ATOMIC_ACQUIRE);
    }
    return false;
}

APTH_API int apth_rdma_wait(struct ibv_cq *cq, uint64_t wr_id,
                                  struct ibv_wc *wc)
{
    struct apth_rdma_poller *p = &GLOBAL_RDMA_POLLER;

    /* Fail-fast if this CQ lost completions due to stash overflow. */
    if (rdma_cq_is_faulted(p, cq))
    {
        errno = EOVERFLOW;
        return -1;
    }

    /* ---- Check stash first: another thread's fast-path may have
     *      already polled our CQE and stashed it. ---- */
    if (__atomic_load_n(&p->stash_count, __ATOMIC_ACQUIRE) > 0)
    {
        lll_internal_lock(&p->waiter_lock);
        for (int i = 0; i < p->stash_count; i++)
        {
            if (p->stash[i].cq == cq && p->stash[i].wc.wr_id == wr_id)
            {
                if (wc)
                    *wc = p->stash[i].wc;
                /* Remove from stash by swapping with last. */
                p->stash_count--;
                if (i < p->stash_count)
                    p->stash[i] = p->stash[p->stash_count];
                lll_internal_unlock(&p->waiter_lock);
                return 0;
            }
        }
        lll_internal_unlock(&p->waiter_lock);
    }

    /* ---- Fast path: poll CQ once (~10ns userspace read). ---- */
    struct ibv_wc local_wc;
    int n = ibv_poll_cq(cq, 1, &local_wc);
    if (n > 0)
    {
        if (local_wc.wr_id == wr_id)
        {
            if (wc)
                *wc = local_wc;
            return 0; /* Zero-yield fast path */
        }
        /* CQE belongs to another wr_id — forward to poller/stash
         * instead of dropping it (ibv_poll_cq is destructive). */
        poller_match_completions(cq, &local_wc, 1);
    }

    /* ---- Slow path: register with poller, block until completion ---- */
    apth_t self = CUR_APTH;
    if (self == NULL)
        return -1; /* Can't yield from scheduler context */

    struct apth_event_st ev;
    memset(&ev, 0, sizeof(ev));
    ev.ev_type = APTH_EVENT_TYPE_RDMA;
    ev.ev_status = APTH_EV_STATUS_PENDING;
    ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    ev.ev_args.RDMA.cq = cq;
    ev.ev_args.RDMA.wr_id = wr_id;
    ev.ev_args.RDMA.wc_out = wc;

    apth_event_list_add(&self->event_list, &ev);

    /* DEDICATED threads use futex blocking; M:N threads yield to scheduler. */
    volatile int rdma_futex_val = 0;
    volatile int *futex_ptr = self->is_dedicated ? &rdma_futex_val : NULL;

    if (rdma_poller_submit(&ev, self, SCHED_OF(self),
                            cq, wr_id, wc, futex_ptr) < 0)
    {
        list_remove(&ev.elem);
        return -1;
    }

    if (self->is_dedicated)
    {
        /* DEDICATED thread: block on futex until poller wakes us.
         * The poller sets rdma_futex_val=1 and FUTEX_WAKE when done.
         *
         * Hard timeout: if the poller silently fails to wake us (race in
         * waiter registration vs CQE processing, lock contention, etc.),
         * an unbounded FUTEX_WAIT hangs the calling thread forever. We've
         * seen this manifest as a JVM hang where no FETCHING-wait timeout
         * fires (the stuck thread is the FETCHER itself, not a waiter).
         * After 30 seconds with no wake, give up and return -1.
         *
         * Before returning, we MUST deactivate our waiter slot in the
         * global poller — otherwise a delayed completion would later
         * write through w->futex_ptr to our stack memory after we've
         * returned (use-after-return). The waiter slot identifies us by
         * (cq, wr_id) which is unique to this RDMA operation. */
        struct timespec ts = {30, 0};
        int rc = 0;
        while (__atomic_load_n(&rdma_futex_val, __ATOMIC_ACQUIRE) == 0)
        {
            int r = syscall(SYS_futex, &rdma_futex_val, FUTEX_WAIT_PRIVATE,
                            0, &ts, NULL, 0);
            if (r == -1 && errno == ETIMEDOUT) {
                /* Atomically deactivate our waiter entry. */
                lll_internal_lock(&p->waiter_lock);
                int wcount = atomic_load_acquire(&p->waiter_count);
                bool found = false;
                for (int i = 0; i < wcount; i++) {
                    struct apth_rdma_waiter *w = &p->waiters[i];
                    if (w->active && w->cq == cq && w->wr_id == wr_id &&
                        w->futex_ptr == &rdma_futex_val) {
                        w->active = false;
                        w->futex_ptr = NULL;  /* poller will skip wake */
                        found = true;
                        break;
                    }
                }
                lll_internal_unlock(&p->waiter_lock);
                apth_debug("apth_rdma_wait: futex timeout (30s) cq=%p wr_id=%lu — %s",
                           cq, (unsigned long)wr_id,
                           found ? "deactivated waiter" : "waiter already gone");
                rc = -1;
                break;
            }
            /* Spurious wakeup or normal wake — recheck loop. */
        }
        if (rc != 0) {
            list_remove(&ev.elem);
            return rc;
        }
    }
    else
    {
        /* M:N thread: yield to scheduler.  The poller will mark
         * ev.ev_status = OCCURRED and wake our scheduler. */
        atomic_store_release(&self->state, APTH_STATE_WAITING);
        self->yield_reason = APTH_YIELD_REASON_WAIT;
        apth_yield();
    }

    /* Resumed. */
    list_remove(&ev.elem);

    return (ev.ev_status == APTH_EV_STATUS_OCCURRED) ? 0 : -1;
}

APTH_API int apth_rdma_wait_batch(struct ibv_cq *cq,
                                        uint64_t *wr_ids, int count,
                                        struct ibv_wc *wcs)
{
    if (count <= 0)
        return 0;

    struct apth_rdma_poller *p = &GLOBAL_RDMA_POLLER;
    if (rdma_cq_is_faulted(p, cq))
    {
        errno = EOVERFLOW;
        return -1;
    }

    apth_t self = CUR_APTH;
    if (self == NULL)
        return -1;

    /* Try to collect all completions without yielding first. */
    struct ibv_wc local_wcs[64];
    int remaining = count;
    bool found[count];
    memset(found, 0, sizeof(bool) * count);

    /* Quick poll pass. */
    int batch = (count < 64) ? count : 64;
    int n = ibv_poll_cq(cq, batch, local_wcs);
    for (int i = 0; i < n; i++)
    {
        bool matched = false;
        for (int j = 0; j < count; j++)
        {
            if (!found[j] && local_wcs[i].wr_id == wr_ids[j])
            {
                if (wcs)
                    wcs[j] = local_wcs[i];
                found[j] = true;
                remaining--;
                matched = true;
                break;
            }
        }
        /* Forward unmatched CQEs to poller/stash to prevent data loss. */
        if (!matched)
            poller_match_completions(cq, &local_wcs[i], 1);
    }

    if (remaining == 0)
        return 0; /* All completed on fast path */

    /* Slow path: register remaining with poller and block. */
    struct apth_event_st evs[count];
    int submitted = 0;

    /* DEDICATED threads use a single futex for the batch: the poller
     * wakes us on each completion, but we only unblock when ALL are done. */
    volatile int batch_futex_val = 0;
    volatile int *futex_ptr = self->is_dedicated ? &batch_futex_val : NULL;

    for (int j = 0; j < count; j++)
    {
        if (found[j])
            continue;

        memset(&evs[j], 0, sizeof(evs[j]));
        evs[j].ev_type = APTH_EVENT_TYPE_RDMA;
        evs[j].ev_status = APTH_EV_STATUS_PENDING;
        evs[j].ev_goal = APTH_GOAL_UNTIL_OCCURRED;
        evs[j].ev_args.RDMA.cq = cq;
        evs[j].ev_args.RDMA.wr_id = wr_ids[j];
        evs[j].ev_args.RDMA.wc_out = wcs ? &wcs[j] : NULL;

        apth_event_list_add(&self->event_list, &evs[j]);

        if (rdma_poller_submit(&evs[j], self, SCHED_OF(self),
                                cq, wr_ids[j],
                                wcs ? &wcs[j] : NULL, futex_ptr) == 0)
            submitted++;
    }

    if (submitted == 0)
    {
        /* Failed to submit any — clean up event list. */
        for (int j = 0; j < count; j++)
        {
            if (!found[j])
                list_remove(&evs[j].elem);
        }
        return -1;
    }

    if (self->is_dedicated)
    {
        /* DEDICATED: wait on futex, recheck all events on each wake.
         * The poller wakes us (futex_val=1) on every completion match. */
        for (;;)
        {
            bool all_done = true;
            for (int j = 0; j < count; j++)
            {
                if (!found[j] &&
                    __atomic_load_n(&evs[j].ev_status, __ATOMIC_ACQUIRE)
                        == APTH_EV_STATUS_PENDING)
                {
                    all_done = false;
                    break;
                }
            }
            if (all_done)
                break;

            /* Reset and wait for next wake. */
            __atomic_store_n(&batch_futex_val, 0, __ATOMIC_RELEASE);
            syscall(SYS_futex, &batch_futex_val, FUTEX_WAIT_PRIVATE,
                    0, NULL, NULL, 0);
        }
    }
    else
    {
        /* M:N: yield to scheduler. The event scan detects when all
         * RDMA events are OCCURRED (since all are in our event_list). */
        atomic_store_release(&self->state, APTH_STATE_WAITING);
        self->yield_reason = APTH_YIELD_REASON_WAIT;
        apth_yield();
    }

    /* Clean up. */
    for (int j = 0; j < count; j++)
    {
        if (!found[j])
            list_remove(&evs[j].elem);
    }

    /* Check that all completed. */
    for (int j = 0; j < count; j++)
    {
        if (!found[j] && evs[j].ev_status != APTH_EV_STATUS_OCCURRED)
            return -1;
    }

    return 0;
}

#endif /* APTH_USE_RDMA */
