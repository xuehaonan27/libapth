#include "apth_event.h"
#include "hook_libc/hooked_funcs.h"
#include "internal/apth_cancel.h"
#include "internal/apth_fd.h"
#include "internal/apth_fd_slot.h"
#include "internal/apth_epoll_waiter.h"
#include "internal/apth_signal.h"
#include "internal/apth_state.h"
#include "internal/apth_time.h"
#include "utils/lll.inline.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include "utils/atomic_wrapper.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>

// Fast waiter allocation from pool
static struct apth_epoll_waiter *alloc_waiter(apth_sched_t sched)
{
    if (!list_empty(&sched->free_waiters))
    {
        struct list_elem *e = list_pop_front(&sched->free_waiters);
        return apth_epoll_waiter_list_entry(e);
    }

    // Pool exhausted, fall back to malloc
    sched->waiter_pool_allocated++;
    if (sched->waiter_pool_allocated > APTH_WAITER_POOL_SIZE)
    {
        apth_debug("WARNING: waiter pool exhausted, using malloc (count=%d)",
                   sched->waiter_pool_allocated);
    }
    return (struct apth_epoll_waiter *)malloc(sizeof(struct apth_epoll_waiter));
}

static void free_waiter(apth_sched_t sched, struct apth_epoll_waiter *w)
{
    // Check if this waiter is from the pool
    if (w >= sched->waiter_pool && w < sched->waiter_pool + APTH_WAITER_POOL_SIZE)
    {
        // Return to pool
        list_push_back(&sched->free_waiters, &w->elem);
    }
    else
    {
        // Was allocated with malloc
        free(w);
        sched->waiter_pool_allocated--;
    }
}

// ==================== FD to apths mapping ====================

// Register a (apth, event) pair to slot of fd.
// If fd is waited for first time, register it to epoll as well.
static int epoll_map_add_waiter(apth_sched_t sched, int fd, apth_t th, apth_event_t ev)
{
    if (fd < 0 || fd >= APTH_EPOLL_FD_SLOT_TABLE_SIZE)
        return -1;
    if (ev->epoll_registered)
        return 0;

    struct apth_epoll_fd_slot *slot = &sched->fd_slot_table[fd];

    // Create waiter entry.
    // TODO: more efficient memory allocation, and reuse memory space.
    struct apth_epoll_waiter *w = alloc_waiter(sched);
    if (w == NULL)
        return -1;
    w->th = th;
    w->ev = ev;

    // Calculate waiter's event mask
    uint32_t needed = 0;
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
    {
        needed |= EPOLLIN;
        slot->readable_count++;
    }
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
    {
        needed |= EPOLLOUT;
        slot->writeable_count++;
    }
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
    {
        needed |= EPOLLPRI;
        slot->exception_count++;
    }

    // Add to waiter list
    list_push_back(&slot->waiters, &w->elem);
    slot->waiter_count++;

    // Update aggregation mask based on refcounts
    uint32_t old_aggregate = slot->aggregate_events;
    slot->aggregate_events = 0;
    if (slot->readable_count > 0)
        slot->aggregate_events |= EPOLLIN;
    if (slot->writeable_count > 0)
        slot->aggregate_events |= EPOLLOUT;
    if (slot->exception_count > 0)
        slot->aggregate_events |= EPOLLPRI;

    // If this is the first waiter, link to active_fd_slots and register to epoll
    if (!slot->registered)
    {
        list_push_back(&sched->active_fd_slots, &slot->elem);
        sched->active_fd_count++;

        struct epoll_event ee;
        ee.events = slot->aggregate_events;
        ee.data.fd = fd; // Using fd instead of ptr, because one fd corresponds to multiple apths
        int rc = epoll_ctl(sched->epoll_fd, EPOLL_CTL_ADD, fd, &ee);
        if (rc < 0)
        {
            // Fail to register, clear
            list_remove(&w->elem);
            slot->waiter_count--;
            slot->aggregate_events = old_aggregate;
            if (slot->waiter_count == 0)
            {
                list_remove(&slot->elem);
                sched->active_fd_count--;
            }
            free_waiter(sched, w);
            return -1;
        }
        slot->registered = true;
    }
    else if (slot->aggregate_events != old_aggregate)
    {
        // Event mask changed (e.g. previous is EPOLLIN, and now EPOLLOUT is added).
        // We need to MOD this event.
        struct epoll_event ee;
        ee.events = slot->aggregate_events;
        ee.data.fd = fd;
        epoll_ctl(sched->epoll_fd, EPOLL_CTL_MOD, fd, &ee);
    }

    ev->epoll_registered = true;

    return 0;
}

// Remove an (apth, event) pair from slot of fd.
// If this is the last waiter, then unregister it from epoll
static void epoll_map_remove_waiter(apth_sched_t sched, int fd, apth_t th, apth_event_t ev)
{
    if (fd < 0 || fd >= APTH_EPOLL_FD_SLOT_TABLE_SIZE)
        return;
    if (!ev->epoll_registered)
        return;

    struct apth_epoll_fd_slot *slot = &sched->fd_slot_table[fd];

    // Find and remove correspond waiter from the list
    FOR_ELEMENT_IN_LIST(slot->waiters, e)
    {
        struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
        if (w->th == th && w->ev == ev)
        {
            // Decrement refcounts based on what this waiter was waiting for
            if (ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
                slot->readable_count--;
            if (ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
                slot->writeable_count--;
            if (ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
                slot->exception_count--;

            list_remove(&w->elem);
            free_waiter(sched, w);
            slot->waiter_count--;
            break;
        }
    }

    if (slot->waiter_count == 0)
    {
        // Last waiter removed, unregister from epoll
        if (slot->registered)
        {
            epoll_ctl(sched->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
            slot->registered = false;
            list_remove(&slot->elem);
            sched->active_fd_count--;
        }
        slot->aggregate_events = 0;
        slot->readable_count = 0;
        slot->writeable_count = 0;
        slot->exception_count = 0;
    }
    else
    {
        // Recalculate aggregate mask from refcounts (O(1) instead of O(n))
        uint32_t new_aggregate = 0;
        if (slot->readable_count > 0)
            new_aggregate |= EPOLLIN;
        if (slot->writeable_count > 0)
            new_aggregate |= EPOLLOUT;
        if (slot->exception_count > 0)
            new_aggregate |= EPOLLPRI;

        if (new_aggregate != slot->aggregate_events)
        {
            slot->aggregate_events = new_aggregate;
            struct epoll_event ee;
            ee.events = new_aggregate;
            ee.data.fd = fd;
            epoll_ctl(sched->epoll_fd, EPOLL_CTL_MOD, fd, &ee);
        }
    }

    ev->epoll_registered = false;
}

// We fd return by epoll is ready, wake up all matching waiters.
// Returns count of apths waked.
static int epoll_map_wake_fd(apth_sched_t sched, int fd, uint32_t revents)
{
    if (fd < 0 || fd >= APTH_EPOLL_FD_SLOT_TABLE_SIZE)
        return 0;

    struct apth_epoll_fd_slot *slot = &sched->fd_slot_table[fd];
    int waked = 0;

    // Tranverse all waiters of this fd, and check satisfied event
    struct list_elem *e = list_begin(&slot->waiters);
    while (e != list_end(&slot->waiters))
    {
        struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
        struct list_elem *next = list_next(e); // save, because waiter might be removed

        bool matched = false;
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE) && (revents & EPOLLIN))
        {
            matched = true;
            slot->readable_count--;
        }
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE) && (revents & EPOLLOUT))
        {
            matched = true;
            slot->writeable_count--;
        }
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION) && (revents & EPOLLPRI))
        {
            matched = true;
            slot->exception_count--;
        }

        // EPOLLERR and EPOLLHUP always matches.
        // If fd errs or peer closes, then all waiters need to be waked.
        if (revents & (EPOLLERR | EPOLLHUP))
            matched = true;

        if (matched)
        {
            // Mark the event as OCCURRED.
            w->ev->ev_status = APTH_EV_STATUS_OCCURRED;
            apth_debug("[epoll] fd=%d event occurred for apth \"%s\"", fd, w->th->name);

            // Remove from waiter list.
            list_remove(&w->elem);
            slot->waiter_count--;

            // Remember apth need to be waked, but do not move it here.
            // An apth might have several events. Moving the apth should be handled
            // outside of this function
            free_waiter(sched, w);
            waked++;
        }

        e = next;
    }

    // If all waiters are removed, then unregister from epoll
    if (slot->waiter_count == 0 && slot->registered)
    {
        epoll_ctl(sched->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        slot->registered = false;
        list_remove(&slot->elem);
        sched->active_fd_count--;
        slot->aggregate_events = 0; // This cost much!
        slot->readable_count = 0;
        slot->writeable_count = 0;
        slot->exception_count = 0;
    }
    else if (waked > 0)
    {
        // Calculate event mask again
        uint32_t new_aggregate = 0;
        if (slot->readable_count > 0)
            new_aggregate |= EPOLLIN;
        if (slot->writeable_count > 0)
            new_aggregate |= EPOLLOUT;
        if (slot->exception_count > 0)
            new_aggregate |= EPOLLPRI;

        if (new_aggregate != slot->aggregate_events)
        {
            slot->aggregate_events = new_aggregate;
            struct epoll_event ee;
            ee.events = new_aggregate;
            ee.data.fd = fd;
            epoll_ctl(sched->epoll_fd, EPOLL_CTL_MOD, fd, &ee);
        }
    }

    return waked;
}

// Fail ALL waiters for a given fd on this scheduler.
// Called when processing pending fd close notifications.
// Must be called from the scheduler's own thread context (event manager).
static void epoll_map_fail_all_waiters_for_fd(apth_sched_t sched, int fd)
{
    if (fd < 0 || fd >= APTH_EPOLL_FD_SLOT_TABLE_SIZE)
        return;

    struct apth_epoll_fd_slot *slot = &sched->fd_slot_table[fd];

    if (slot->waiter_count == 0)
        return;

    apth_debug("[fd_close] failing all waiters for fd=%d on sched %d", fd, sched->id);

    // Traverse and fail all waiters
    struct list_elem *e = list_begin(&slot->waiters);
    while (e != list_end(&slot->waiters))
    {
        struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
        struct list_elem *next = list_next(e);

        w->ev->ev_status = APTH_EV_STATUS_FAILED;
        w->ev->epoll_registered = false;

        apth_debug("[fd_close] fd=%d event FAILED for apth \"%s\"", fd, w->th->name);

        list_remove(&w->elem);
        free_waiter(sched, w);

        e = next;
    }

    slot->waiter_count = 0;
    slot->aggregate_events = 0;

    // Remove from active list and epoll if registered
    if (slot->registered)
    {
        epoll_ctl(sched->epoll_fd, EPOLL_CTL_DEL, fd, NULL); // ignore error, fd may already be closed
        slot->registered = false;
        list_remove(&slot->elem);
        sched->active_fd_count--;
    }
}

// Process pending fd close notifications. Called at start of event manager.
// Drains the pending list and fails all local waiters for each closed fd.
APTH_INTERNAL void apth_sched_process_pending_fd_closes(apth_sched_t sched)
{
    if (atomic_load_acquire(&sched->pending_fd_close_count) == 0)
        return;

    int local_fds[APTH_PENDING_FD_CLOSE_MAX];
    int local_count;

    lll_internal_lock(&sched->pending_fd_close_lock);
    local_count = atomic_load_acquire(&sched->pending_fd_close_count);
    if (local_count > APTH_PENDING_FD_CLOSE_MAX)
        local_count = APTH_PENDING_FD_CLOSE_MAX;
    memcpy(local_fds, sched->pending_fd_close_fds, local_count * sizeof(int));
    atomic_store_release(&sched->pending_fd_close_count, 0);
    lll_internal_unlock(&sched->pending_fd_close_lock);

    for (int i = 0; i < local_count; i++)
    {
        epoll_map_fail_all_waiters_for_fd(sched, local_fds[i]);
    }
}

APTH_INTERNAL bool apth_state_matches_event_goal(apth_state_t state, apth_goal_t goal)
{
    // With the new state management, we don't need to check for uncommitted state
    // because TERMINATED state is set atomically while holding the queue lock.
    // For other states, we use simple atomic stores.
    return ((int)state == (int)goal);
}

APTH_INTERNAL void apth_sched_eventmanager_epoll(apth_sched_t sched, apth_time_t *now, bool dopoll)
{
    apth_debug("enter in %s mode", dopoll ? "polling" : "waiting");

    for (;;)
    {
        bool loop_repeat = false;

        // ==================== Phase 0: process pending fd close notifications ====================
        apth_sched_process_pending_fd_closes(sched);

        // ==================== Phase 1: tranverse waiting queue ====================
        // Handle non-I/O events (timer, signal, tid, func)
        // Register FD events to epoll mapping table

        apth_time_t nexttimer_value;
        apth_time_set(&nexttimer_value, APTH_TIME_ZERO);
        apth_event_t nexttimer_ev = APTH_EVENT_NULL;
        apth_t nexttimer_th = APTH_NULL;
        bool has_timer = false;
        size_t notified_ths = 0;

// Collect apths that need to be waked, avoiding operating waked_queue
// while still holding lock of waiting_queue, which is deadlock-prone.
// Use a temporary array here.
#define MAX_WAKE_BATCH 128
        apth_t wake_batch[MAX_WAKE_BATCH];
        int wake_count = 0;

        lll_internal_lock(&THQUEUE(sched, waiting)->th_list_lock);

        FOR_ELEMENT_IN_LIST(THQUEUE(sched, waiting)->th_list, e)
        {
            apth_t th = apth_t_list_entry(e);
            bool any_occurred = false;

            // Check cancelation request
            if (atomic_load_acquire(&th->cancelreq))
                any_occurred = true;

            if (list_empty(&th->event_list))
                goto check_wake;

            FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
            {
                apth_event_t event = apth_event_t_list_entry(ev_e);
                if (event->ev_status != APTH_EV_STATUS_PENDING)
                {
                    // Previous epoll_map_wake_fd might already marked this event
                    any_occurred = true;
                    continue;
                }

                switch (event->ev_type)
                {
                case APTH_EVENT_TYPE_FD:
                    // Register to epoll mapping table (if not registered yet)
                    // If registration fails (e.g., fd was closed), mark the event as FAILED
                    // so the apth doesn't wait forever on a dead fd.
                    if (epoll_map_add_waiter(sched, event->ev_args.FD.fd, th, event) < 0)
                    {
                        event->ev_status = APTH_EV_STATUS_FAILED;
                        any_occurred = true;
                        apth_debug("[epoll] fd=%d registration failed for apth \"%s\"",
                                   event->ev_args.FD.fd, th->name);
                    }
                    break;

                case APTH_EVENT_TYPE_SELECT:
                    // SELECT event: register every fd in fd_set to epoll separately.
                    // The new method need support from hooked syscall `select`
                    // Or we could fallback to old selection check.
                    {
                        // Fallback: do a quick raw select check to SELECT event
                        struct timeval zero_tv = {0, 0};
                        fd_set trfds, twfds, tefds;
                        fd_set *prfds = NULL, *pwfds = NULL, *pefds = NULL;
                        if (event->ev_args.SELECT.rfds)
                        {
                            memcpy(&trfds, event->ev_args.SELECT.rfds, sizeof(fd_set));
                            prfds = &trfds;
                        }
                        if (event->ev_args.SELECT.wfds)
                        {
                            memcpy(&twfds, event->ev_args.SELECT.wfds, sizeof(fd_set));
                            pwfds = &twfds;
                        }
                        if (event->ev_args.SELECT.efds)
                        {
                            memcpy(&tefds, event->ev_args.SELECT.efds, sizeof(fd_set));
                            pefds = &tefds;
                        }

                        int rc;
                        while ((rc = apth_func_raw(select)(event->ev_args.SELECT.nfd, prfds, pwfds, pefds, &zero_tv)) < 0 && errno == EINTR)
                            ;
                        if (rc > 0)
                        {
                            // some fd ready
                            int n = apth_util_fds_select(event->ev_args.SELECT.nfd,
                                                         event->ev_args.SELECT.rfds, prfds,
                                                         event->ev_args.SELECT.wfds, pwfds,
                                                         event->ev_args.SELECT.efds, pefds);
                            if (event->ev_args.SELECT.n)
                                *(event->ev_args.SELECT.n) = n;
                            event->ev_status = APTH_EV_STATUS_OCCURRED;
                            any_occurred = true;
                        }
                        else if (rc < 0)
                        {
                            event->ev_status = APTH_EV_STATUS_FAILED;
                            any_occurred = true;
                        }
                        // rc == 0: not ready, check in next event manager
                    }
                    break;

                case APTH_EVENT_TYPE_SIGS:
                    // Signal check, do this here, no need for second loop
                    for (int sig = 1; sig < APTH_NSIG; sig++)
                    {
                        if (sigismember(event->ev_args.SIGS.sigs, sig))
                        {
                            lll_internal_lock(&th->siglock);
                            if (sigismember(&th->sigpending, sig))
                            {
                                if (event->ev_args.SIGS.sig)
                                    *(event->ev_args.SIGS.sig) = sig;
                                sigdelset(&th->sigpending, sig);
                                th->sigpendcnt--;
                                lll_internal_unlock(&th->siglock);
                                event->ev_status = APTH_EV_STATUS_OCCURRED;
                                any_occurred = true;
                                break;
                            }
                            lll_internal_unlock(&th->siglock);
                        }
                    }
                    break;

                case APTH_EVENT_TYPE_TIME:
                    if (apth_time_cmp(&event->ev_args.TIME.tv, now) < 0)
                    {
                        event->ev_status = APTH_EV_STATUS_OCCURRED;
                        any_occurred = true;
                    }
                    else
                    {
                        if (!has_timer || apth_time_cmp(&event->ev_args.TIME.tv, &nexttimer_value) < 0)
                        {
                            apth_time_set(&nexttimer_value, &event->ev_args.TIME.tv);
                            nexttimer_ev = event;
                            nexttimer_th = th;
                            has_timer = true;
                        }
                    }
                    break;

                case APTH_EVENT_TYPE_TID:
                    if ((event->ev_args.TID.tid == NULL && thqueue_size(THQUEUE(sched, terminated)) != 0) ||
                        (event->ev_args.TID.tid != NULL &&
                         apth_state_matches_event_goal(atomic_load_acquire(&event->ev_args.TID.tid->state), event->ev_goal)))
                    {
                        event->ev_status = APTH_EV_STATUS_OCCURRED;
                        any_occurred = true;
                    }
                    break;

                case APTH_EVENT_TYPE_FUNC:
                    if (event->ev_args.FUNC.func(event->ev_args.FUNC.arg))
                    {
                        event->ev_status = APTH_EV_STATUS_OCCURRED;
                        any_occurred = true;
                    }
                    else
                    {
                        apth_time_t tv;
                        apth_time_set(&tv, now);
                        apth_time_add(&tv, &event->ev_args.FUNC.tv);
                        if (!has_timer || apth_time_cmp(&tv, &nexttimer_value) < 0)
                        {
                            apth_time_set(&nexttimer_value, &tv);
                            nexttimer_ev = event;
                            nexttimer_th = th;
                            has_timer = true;
                        }
                    }
                    break;

                default:
                    break;
                }
            }

        check_wake:
            if (any_occurred)
            {
                notified_ths++;
                if (wake_count < MAX_WAKE_BATCH)
                    wake_batch[wake_count++] = th;
            }
        }

        lll_internal_unlock(&THQUEUE(sched, waiting)->th_list_lock);

        // Move apth discovered during phase 1 from waiting queue to waked queue.
        // Loop until we have transferred all of them: if wake_count hit MAX_WAKE_BATCH
        // on this pass we may have more waiting, so we re-scan after draining.
        do
        {
            for (int i = 0; i < wake_count; i++)
            {
                apth_t th = wake_batch[i];
                // Remove ALL fd wait event of this apth from epoll mapping table.
                FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
                {
                    apth_event_t event = apth_event_t_list_entry(ev_e);
                    if (event->ev_type == APTH_EVENT_TYPE_FD)
                        epoll_map_remove_waiter(sched, event->ev_args.FD.fd, th, event);
                }
                // Move to waked queue
                atomic_store_release(&th->state, APTH_STATE_WAKED);
                transfer_th(th, THQUEUE(sched, waiting), THQUEUE(sched, waked));
            }

            // If the batch was full, there may be more apths in the waiting queue
            // that were skipped. Re-scan to collect and transfer them.
            if (wake_count == MAX_WAKE_BATCH)
            {
                wake_count = 0;
                lll_internal_lock(&THQUEUE(sched, waiting)->th_list_lock);
                FOR_ELEMENT_IN_LIST(THQUEUE(sched, waiting)->th_list, e)
                {
                    apth_t th = apth_t_list_entry(e);
                    bool any_occurred = atomic_load_acquire(&th->cancelreq);
                    if (!any_occurred)
                    {
                        FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
                        {
                            apth_event_t event = apth_event_t_list_entry(ev_e);
                            if (event->ev_status != APTH_EV_STATUS_PENDING)
                            {
                                any_occurred = true;
                                break;
                            }
                        }
                    }
                    if (any_occurred && wake_count < MAX_WAKE_BATCH)
                        wake_batch[wake_count++] = th;
                }
                lll_internal_unlock(&THQUEUE(sched, waiting)->th_list_lock);
                notified_ths += wake_count;
            }
            else
            {
                break;
            }
        } while (wake_count > 0);

        // If there's apth waked during phase 1, then use 0 timeout for epoll_wait
        if (notified_ths > 0)
            dopoll = true;

        // ==================== Phase 2: epoll_wait handle I/O events ====================

        int timeout_ms;
        // OPTIMIZATION: Even if we have ready work (dopoll=true), still check I/O
        // with a very short timeout if there are active FDs waiting. This prevents
        // busy-waiting and allows I/O to complete while we have work to do.
        // The key insight: checking I/O with timeout=0 is cheap (just a syscall),
        // and it can wake up blocked threads immediately.
        if (dopoll && sched->active_fd_count > 0)
        {
            // Quick poll for I/O even when we have ready work
            timeout_ms = 0;
        }
        else if (dopoll)
        {
            // Ready work but no I/O to check, skip epoll entirely
            timeout_ms = -1; // Signal to skip epoll_wait
        }
        else if (has_timer)
        {
            apth_time_t diff;
            apth_time_set(&diff, &nexttimer_value);
            apth_time_sub(&diff, now);
            timeout_ms = (int)(diff.tv_sec * 1000 + diff.tv_usec / 1000);
            if (timeout_ms < 0)
                timeout_ms = 0;
            if (timeout_ms > 60000)
                timeout_ms = 60000; // max 60s
        }
        else
        {
            // No timer, no active FDs: block briefly so we don't busy-spin.
            // The wake_eventfd will interrupt us if new work arrives.
            timeout_ms = 10;
        }

        // Call epoll_wait whenever we have FDs to watch, a timer to honor,
        // or we need to block while idle (wake_eventfd is always registered).
        // Skip epoll_wait only if timeout_ms == -1 (dopoll=true with no active FDs)
        if (timeout_ms >= 0 && (sched->active_fd_count > 0 || has_timer || !dopoll))
        {
            struct epoll_event ep_events[64];
            int nready = epoll_wait(sched->epoll_fd, ep_events, 64, timeout_ms);

            if (nready > 0)
            {
                // handle ready fd
                for (int i = 0; i < nready; i++)
                {
                    int ready_fd = ep_events[i].data.fd;
                    uint32_t revents = ep_events[i].events; // This cost much!

                    // Drain the wake eventfd (used to interrupt idle epoll_wait).
                    // Just consume the counter; no apth needs to be woken for it.
                    if (ready_fd == sched->wake_eventfd)
                    {
                        // Drain the wake eventfd counter so it becomes non-readable again.
                        uint64_t val;
                        ssize_t __ignored = apth_func_raw(read)(sched->wake_eventfd, &val, sizeof(val));
                        (void)__ignored;
                        continue;
                    }

                    // wake all waiters
                    epoll_map_wake_fd(sched, ready_fd, revents);
                }

                // epoll_map_wake_fd already marked event as OCCURRED,
                // but not move apth to waked_queue yet.
                // Now traverse waiting queue and move apths with OCCURRED events,
                // looping until all have been transferred (handles >MAX_WAKE_BATCH case).
                do
                {
                    wake_count = 0;
                    lll_internal_lock(&THQUEUE(sched, waiting)->th_list_lock);
                    FOR_ELEMENT_IN_LIST(THQUEUE(sched, waiting)->th_list, e)
                    {
                        apth_t th = apth_t_list_entry(e);
                        bool should_wake = false;
                        FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
                        {
                            apth_event_t event = apth_event_t_list_entry(ev_e);
                            if (event->ev_status != APTH_EV_STATUS_PENDING)
                            {
                                should_wake = true;
                                break;
                            }
                        }
                        if (should_wake && wake_count < MAX_WAKE_BATCH)
                            wake_batch[wake_count++] = th;
                    }
                    lll_internal_unlock(&THQUEUE(sched, waiting)->th_list_lock);

                    for (int i = 0; i < wake_count; i++)
                    {
                        apth_t th = wake_batch[i];
                        // Clear other epoll registrations of the apth
                        // No need for caring about other types of events, just take care of FD events
                        // we must remove these FD events from scheduler's waiter list
                        FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
                        {
                            apth_event_t event = apth_event_t_list_entry(ev_e);
                            if (event->ev_type == APTH_EVENT_TYPE_FD &&
                                event->ev_status == APTH_EV_STATUS_PENDING)
                                epoll_map_remove_waiter(sched, event->ev_args.FD.fd, th, event);
                        }
                        atomic_store_release(&th->state, APTH_STATE_WAKED);
                        transfer_th(th, THQUEUE(sched, waiting), THQUEUE(sched, waked));
                    }
                } while (wake_count == MAX_WAKE_BATCH);
            }
            else if (nready == 0 && !dopoll && has_timer)
            {
                // epoll_wait timeout, the timer MIGHT have timed out.
                if (nexttimer_ev != NULL)
                {
                    if (nexttimer_ev->ev_type == APTH_EVENT_TYPE_FUNC)
                    {
                        // Implicit timer of FUNC event, check again
                        loop_repeat = true;
                    }
                    else
                    {
                        // Explicit timer event
                        nexttimer_ev->ev_status = APTH_EV_STATUS_OCCURRED;
                        apth_debug("[timeout] event occurred for apth \"%s\"", nexttimer_th->name);
                        // Move to waked queue
                        FOR_ELEMENT_IN_LIST(nexttimer_th->event_list, ev_e)
                        {
                            apth_event_t event = apth_event_t_list_entry(ev_e);
                            if (event->ev_type == APTH_EVENT_TYPE_FD)
                                epoll_map_remove_waiter(sched, event->ev_args.FD.fd, nexttimer_th, event);
                        }
                        atomic_store_release(&nexttimer_th->state, APTH_STATE_WAKED);
                        transfer_th(nexttimer_th, THQUEUE(sched, waiting), THQUEUE(sched, waked));
                    }
                }
            }
        }

        // loop control
        if (loop_repeat)
        {
            apth_time_set(now, APTH_TIME_NOW);
            continue;
        }
        else
        {
            break;
        }
    }

    apth_debug("leave");
}

APTH_INTERNAL void apth_event_list_add(struct list *el, apth_event_t ev)
{
    // TODO: atomicity
    list_push_back(el, &ev->elem);
}

APTH_INTERNAL void apth_event_isolate(apth_event_t ev)
{
    // TODO: atomicity
    list_remove(&ev->elem);
}

APTH_INTERNAL int apth_wait_event_list(struct list *el)
{
    if (list_empty(el))
        return apth_error(-1, EINVAL);

    apth_t self = CUR_APTH;

    apth_debug("apth_wait_events: enter from thread \"%s\"", self->name);

    // Mark all events in the list as still pending
    FOR_ELEMENT_IN_LIST_REF(el, e)
    {
        apth_event_t ev = apth_event_t_list_entry(e);
        ev->ev_status = APTH_EV_STATUS_PENDING;
        apth_debug("apth_wait_events: waiting on event 0x%lx", (unsigned long)ev);
    }

    // Link event list to current thread
    // list_append(&self->event_list, el);
    self->event_list = *el;

    // Move apth into waiting state and transfer control to scheduler
    atomic_store_release(&self->state, APTH_STATE_WAITING);
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    // Check for cancellation
    apth_cancel_point();

    // Unlink events from current thread
    list_init(&self->event_list);

    // Count number of actually occurred (or failed) events
    int nonpending = 0;
    FOR_ELEMENT_IN_LIST_REF(el, e2)
    {
        apth_event_t ev = apth_event_t_list_entry(e2);
        if (ev->ev_status != APTH_EV_STATUS_PENDING)
        {
            apth_debug("apth_wait_event_list: non-pending event 0x%lx", (unsigned long)ev);
            nonpending++;
        }
    }

    // Leave to current thread with number of occurred events
    apth_debug("apth_wait_event_list: leave to thread \"%s\"", self->name);
    return nonpending;
}

APTH_INTERNAL bool apth_wait_event(apth_event_t ev)
{
    if (ev == APTH_EVENT_NULL)
        return apth_error(false, EINVAL);
    apth_t self = CUR_APTH;
    assert(SCHED_OF(self) == CUR_SCHED);
    apth_debug("enter from thread \"%s\"", self->name);

    // Mark the event as still pending
    ev->ev_status = APTH_EV_STATUS_PENDING;

    // Link event into current thread
    apth_event_list_add(&self->event_list, ev);

    // Move thread into waiting state and transfer control to scheduler
    // Move to waiting state
    atomic_store_release(&self->state, APTH_STATE_WAITING);
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    // Check for cancellation
    apth_cancel_point();

    // Unlink event from current thread
    // TODO: atomicity
    list_remove(&ev->elem);
    assert_msg(list_empty(&self->event_list), "insane");

    // Judge whether the event has occurred

    bool result = false;
    if (ev->ev_status != APTH_EV_STATUS_PENDING)
    {
        result = true;
    }
    else
    {
        // TODO: if the scheduler designed to be possible scheduling
        // a waiting apth, then we should yield again here if the
        // event status is still pending
    }

    // Leave to current thread with number of occurred events
    apth_debug("leave to thread \"%s\"", self->name);
    return result;
}
