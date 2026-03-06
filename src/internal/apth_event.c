#include "common.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include "utils/atomic_wrapper.h"
#include <malloc.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
// #include <unistd.h>

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
    struct apth_epoll_waiter *w = (struct apth_epoll_waiter *)malloc(sizeof(*w));
    if (w == NULL)
        return -1;
    w->th = th;
    w->ev = ev;

    // Calculate waiter's event mask
    uint32_t needed = 0;
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
        needed |= EPOLLIN;
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
        needed |= EPOLLOUT;
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
        needed |= EPOLLPRI;

    // Add to waiter list
    list_push_back(&slot->waiters, &w->elem);
    slot->waiter_count++;

    // Update aggregation mask
    uint32_t old_aggregate = slot->aggregate_events;
    slot->aggregate_events |= needed;

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
            free(w);
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
            list_remove(&w->elem);
            free(w); // TODO: better memory allocation and deallocation
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
            list_remove(&slot->elem); // Remove from active_fd_slots
            sched->active_fd_count--;
        }
        slot->aggregate_events = 0;
    }
    else
    {
        // There's still other waiters.
        // We need to calculate mask again, because the removed waiter might be
        // the only one requiring a certain flag.
        uint32_t new_aggregate = 0;
        FOR_ELEMENT_IN_LIST(slot->waiters, e2)
        {
            struct apth_epoll_waiter *w2 = apth_epoll_waiter_list_entry(e2);
            if (w2->ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
                new_aggregate |= EPOLLIN;
            if (w2->ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
                new_aggregate |= EPOLLOUT;
            if (w2->ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
                new_aggregate |= EPOLLPRI;
        }
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
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE) && (revents & (EPOLLIN | EPOLLHUP | EPOLLERR)))
            matched = true;
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE) && (revents & (EPOLLOUT | EPOLLERR)))
            matched = true;
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION) && (revents & (EPOLLPRI | EPOLLERR)))
            matched = true;

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
            free(w); // free the waiter
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
        slot->aggregate_events = 0;
    }
    else if (waked > 0)
    {
        // Calculate event mask again
        uint32_t new_aggregate = 0;
        FOR_ELEMENT_IN_LIST(slot->waiters, e2)
        {
            struct apth_epoll_waiter *w2 = apth_epoll_waiter_list_entry(e2);
            if (w2->ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
                new_aggregate |= EPOLLIN;
            if (w2->ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
                new_aggregate |= EPOLLOUT;
            if (w2->ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
                new_aggregate |= EPOLLPRI;
        }
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
        free(w);

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

    lll_lock(&sched->pending_fd_close_lock, "process_pending_fd_closes");
    local_count = atomic_load_acquire(&sched->pending_fd_close_count);
    if (local_count > APTH_PENDING_FD_CLOSE_MAX)
        local_count = APTH_PENDING_FD_CLOSE_MAX;
    memcpy(local_fds, sched->pending_fd_close_fds, local_count * sizeof(int));
    atomic_store_release(&sched->pending_fd_close_count, 0);
    lll_unlock(&sched->pending_fd_close_lock, "process_pending_fd_closes");

    for (int i = 0; i < local_count; i++)
    {
        epoll_map_fail_all_waiters_for_fd(sched, local_fds[i]);
    }
}

APTH_INTERNAL bool apth_state_matches_event_goal(apth_state_t state, apth_goal_t goal)
{
    // If the state is not committed, then means it's yet to reach goal.
    if (state_is_uncommitted(state))
        return false;

    if (state == APTH_STATE_NEW && goal == APTH_GOAL_UNTIL_TID_NEW)
        return true;
    if (state == APTH_STATE_READY && goal == APTH_GOAL_UNTIL_TID_READY)
        return true;
    if (state == APTH_STATE_WAITING && goal == APTH_GOAL_UNTIL_TID_WAITING)
        return true;
    if (state == APTH_STATE_TERMINATED && goal == APTH_GOAL_UNTIL_TID_DEAD)
        return true;

    return false;
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

        lll_lock(&sched->waiting_queue->th_list_lock, "eventmanager_phase1");

        FOR_ELEMENT_IN_LIST(sched->waiting_queue->th_list, e)
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
                            lll_lock(&th->siglock, "ev_sigs_epoll");
                            if (sigismember(&th->sigpending, sig))
                            {
                                if (event->ev_args.SIGS.sig)
                                    *(event->ev_args.SIGS.sig) = sig;
                                sigdelset(&th->sigpending, sig);
                                th->sigpendcnt--;
                                lll_unlock(&th->siglock, "ev_sigs_epoll");
                                event->ev_status = APTH_EV_STATUS_OCCURRED;
                                any_occurred = true;
                                break;
                            }
                            lll_unlock(&th->siglock, "ev_sigs_epoll");
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
                    if ((event->ev_args.TID.tid == NULL && thqueue_size(sched->terminated_queue) != 0) ||
                        (event->ev_args.TID.tid != NULL &&
                         apth_state_matches_event_goal(state_holder_of(event->ev_args.TID.tid), event->ev_goal)))
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

        lll_unlock(&sched->waiting_queue->th_list_lock, "eventmanager_phase1");

        // Move apth discovered during phase 1 from waiting queue to waked queue.
        // Loop until we have transferred all of them: if wake_count hit MAX_WAKE_BATCH
        // on this pass we may have more waiting, so we re-scan after draining.
        do {
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
                submit_desired_state_to(th, APTH_STATE_WAKED, "eventmanager phase1");
                transfer_th(th, sched->waiting_queue, sched->waked_queue);
            }

            // If the batch was full, there may be more apths in the waiting queue
            // that were skipped. Re-scan to collect and transfer them.
            if (wake_count == MAX_WAKE_BATCH)
            {
                wake_count = 0;
                lll_lock(&sched->waiting_queue->th_list_lock, "eventmanager_phase1_rescan");
                FOR_ELEMENT_IN_LIST(sched->waiting_queue->th_list, e)
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
                lll_unlock(&sched->waiting_queue->th_list_lock, "eventmanager_phase1_rescan");
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
        if (dopoll)
        {
            timeout_ms = 0;
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
            timeout_ms = dopoll ? 0 : 10;
        }

        // Call epoll_wait whenever we have FDs to watch, a timer to honor,
        // or we need to block while idle (wake_eventfd is always registered).
        if (sched->active_fd_count > 0 || has_timer || !dopoll)
        {
            struct epoll_event ep_events[64];
            int nready = epoll_wait(sched->epoll_fd, ep_events, 64, timeout_ms);

            if (nready > 0)
            {
                // handle ready fd
                for (int i = 0; i < nready; i++)
                {
                    int ready_fd = ep_events[i].data.fd;
                    uint32_t revents = ep_events[i].events;

                    // Drain the wake eventfd (used to interrupt idle epoll_wait).
                    // Just consume the counter; no apth needs to be woken for it.
                    if (ready_fd == sched->wake_eventfd)
                    {
                        // Drain the wake eventfd counter so it becomes non-readable again.
                        uint64_t val;
                        ssize_t __ignored = read(sched->wake_eventfd, &val, sizeof(val));
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
                do {
                    wake_count = 0;
                    lll_lock(&sched->waiting_queue->th_list_lock, "eventmanager_phase2_wake");
                    FOR_ELEMENT_IN_LIST(sched->waiting_queue->th_list, e)
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
                    lll_unlock(&sched->waiting_queue->th_list_lock, "eventmanager_phase2_wake");

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
                        submit_desired_state_to(th, APTH_STATE_WAKED, "eventmanager phase2");
                        transfer_th(th, sched->waiting_queue, sched->waked_queue);
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
                        submit_desired_state_to(nexttimer_th, APTH_STATE_WAKED, "eventmanager phase2");
                        transfer_th(nexttimer_th, sched->waiting_queue, sched->waked_queue);
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

static apth_event_t prepare_ev(unsigned long spec MAYBE_UNUSED)
{
    // TODO: use spec

    apth_event_t ev;
    ev = (apth_event_t)malloc(sizeof(struct apth_event_st));
    // TODO: profile average thread holding events and prepare a preallocated event structure pool

    if (ev == NULL)
        return apth_error(APTH_EVENT_NULL, errno);

    // Initialize common ingredients
    ev->ev_status = APTH_EV_STATUS_PENDING;
    ev->epoll_registered = false;

    return ev;
}

APTH_INTERNAL apth_event_t apth_event_fd(unsigned long spec, int fd)
{
    // Filedescriptor event
    if (!apth_util_fd_valid(fd))
        return apth_error(APTH_EVENT_NULL, EBADF);

    apth_event_t ev = prepare_ev(spec);
    ev->ev_type = APTH_EVENT_TYPE_FD;
    ev->ev_goal = (int)(spec & (APTH_GOAL_UNTIL_FD_READABLE |
                                APTH_GOAL_UNTIL_FD_WRITEABLE |
                                APTH_GOAL_UNTIL_FD_EXCEPTION));
    ev->ev_args.FD.fd = fd;
    return ev;
}

APTH_INTERNAL apth_event_t apth_event_select(unsigned long spec, int *n, int nfd,
                                             fd_set *rfds, fd_set *wfds, fd_set *efds)
{
    // Fiedescriptor set select event
    apth_event_t ev = prepare_ev(spec);
    ev->ev_type = APTH_EVENT_TYPE_SELECT;
    ev->ev_goal = (int)(spec & APTH_GOAL_UNTIL_OCCURRED);
    ev->ev_args.SELECT.n = n;
    ev->ev_args.SELECT.nfd = nfd;
    ev->ev_args.SELECT.rfds = rfds;
    ev->ev_args.SELECT.wfds = wfds;
    ev->ev_args.SELECT.efds = efds;
    return ev;
}

// TODO: sigs will drop const identifier, performing a const-cast.
// Note that this is error-prone. If caller submits a temporary variable on the
// stack, and apth_wait_event exceeds scope, then `sigs` and `sig` will become
// dangling pointers. Currently all invocations, `sigs` and `sig` will still
// be on the stack of apths while waiting the event. But this could be an
// error prone situation.
APTH_INTERNAL apth_event_t apth_event_sigs(unsigned long spec, const sigset_t *sigs, int *sig)
{
    // Signal set event
    apth_event_t ev = prepare_ev(spec);
    ev->ev_type = APTH_EVENT_TYPE_SIGS;
    ev->ev_goal = (int)(spec & APTH_GOAL_UNTIL_OCCURRED);
    ev->ev_args.SIGS.sigs = (sigset_t *)sigs;
    ev->ev_args.SIGS.sig = sig;
    return ev;
}

APTH_INTERNAL apth_event_t apth_event_time(unsigned long spec, apth_time_t tv)
{
    // Interrupt request event
    apth_event_t ev = prepare_ev(spec);
    ev->ev_type = APTH_EVENT_TYPE_TIME;
    ev->ev_goal = (int)(spec & APTH_GOAL_UNTIL_OCCURRED);
    ev->ev_args.TIME.tv = tv;
    return ev;
}

APTH_INTERNAL apth_event_t apth_event_mutex(unsigned long spec)
{
    apth_event_t ev = prepare_ev(spec);
    ev->ev_type = APTH_EVENT_TYPE_MUTEX;
    ev->ev_goal = (int)(spec & APTH_GOAL_UNTIL_OCCURRED);
    return ev;
}

APTH_INTERNAL apth_event_t apth_event_cond(unsigned long spec)
{
    apth_event_t ev = prepare_ev(spec);
    ev->ev_type = APTH_EVENT_TYPE_COND;
    ev->ev_goal = (int)(spec & APTH_GOAL_UNTIL_OCCURRED);
    return ev;
}

APTH_INTERNAL apth_event_t apth_event_tid(unsigned long spec, apth_t tid)
{
    // Thread id event
    apth_event_t ev = prepare_ev(spec);
    int goal;
    ev->ev_type = APTH_EVENT_TYPE_TID;
    if (spec & APTH_GOAL_UNTIL_TID_NEW)
        goal = APTH_GOAL_UNTIL_TID_NEW;
    else if (spec & APTH_GOAL_UNTIL_TID_READY)
        goal = APTH_GOAL_UNTIL_TID_READY;
    else if (spec & APTH_GOAL_UNTIL_TID_DEAD)
        goal = APTH_GOAL_UNTIL_TID_DEAD;
    else
        goal = APTH_GOAL_UNTIL_TID_READY; // TODO:check: is this right?
    ev->ev_goal = goal;
    ev->ev_args.TID.tid = tid;

    return ev;
}

APTH_INTERNAL apth_event_t apth_event_func(unsigned long spec, apth_event_custom_func_t func, void *arg, apth_time_t tv)
{
    apth_event_t ev = prepare_ev(spec);
    ev->ev_type = APTH_EVENT_TYPE_FUNC;
    ev->ev_goal = (int)(spec & APTH_GOAL_UNTIL_OCCURRED);
    ev->ev_args.FUNC.func = func;
    ev->ev_args.FUNC.arg = arg;
    ev->ev_args.FUNC.tv = tv;
    return ev;
}

APTH_INTERNAL bool apth_event_free(apth_event_t ev)
{
    if (ev == NULL)
        return apth_error(false, EINVAL);

    free(ev);
    return true;
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

    apth_t self = cur_apth();

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
    // self->state = APTH_STATE_WAITING;
    // We want to submit WAITING state to `self`
    submit_desired_state_to(self, APTH_STATE_WAITING, "apth_wait_event_list");
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
    apth_t self = cur_apth();
    assert(sched_of(self) == cur_sched());
    apth_debug("enter from thread \"%s\"", self->name);

    // Mark the event as still pending
    ev->ev_status = APTH_EV_STATUS_PENDING;

    // Link event into current thread
    apth_event_list_add(&self->event_list, ev);

    // Move thread into waiting state and transfer control to scheduler
    // We want to submit WAITING state to `self`
    submit_desired_state_to(self, APTH_STATE_WAITING, "apth_wait_event");
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