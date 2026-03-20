#include "apth_event.h"
#include "hook_libc/hooked_funcs.h"
#include "internal/apth_cancel.h"
#include "internal/apth_fd.h"
#include "internal/apth_fd_slot.h"
#include "internal/apth_epoll_waiter.h"
#include "internal/apth_reactor.h"
#include "internal/apth_signal.h"
#include "internal/apth_state.h"
#include "internal/apth_time.h"
#include "utils/lll.inline.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include "utils/atomic_wrapper.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <malloc.h>

APTH_INTERNAL bool apth_state_matches_event_goal(apth_state_t state, apth_goal_t goal)
{
    return ((int)state == (int)goal);
}

// The event manager now only handles non-FD events (timer, signal, TID, FUNC, SELECT).
// FD events are handled by the global reactor. The scheduler blocks on its wake_eventfd
// when idle, which the reactor uses to wake it when FD events complete.
APTH_INTERNAL void apth_sched_eventmanager_epoll(apth_sched_t sched, apth_time_t *now, bool dopoll)
{
    apth_debug("enter in %s mode", dopoll ? "polling" : "waiting");

    for (;;)
    {
        bool loop_repeat = false;

        // ==================== Phase 1: traverse waiting queue ====================
        // Handle non-I/O events (timer, signal, tid, func, select)
        // FD events are registered with the global reactor.

        apth_time_t nexttimer_value;
        apth_time_set(&nexttimer_value, APTH_TIME_ZERO);
        apth_event_t nexttimer_ev = APTH_EVENT_NULL;
        apth_t nexttimer_th = APTH_NULL;
        bool has_timer = false;
        size_t notified_ths = 0;

#define MAX_WAKE_BATCH 512
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
                    // Reactor or previous pass already marked this event
                    any_occurred = true;
                    continue;
                }

                switch (event->ev_type)
                {
                case APTH_EVENT_TYPE_FD:
                    // FD events are handled by the global reactor.
                    // Fallback registration for edge cases (e.g., failed eager registration).
                    if (!event->epoll_registered)
                    {
                        if (apth_reactor_add_waiter(sched, event->ev_args.FD.fd, th, event) < 0)
                        {
                            event->ev_status = APTH_EV_STATUS_FAILED;
                            any_occurred = true;
                        }
                    }
                    break;

                case APTH_EVENT_TYPE_SELECT:
                    {
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
                    }
                    break;

                case APTH_EVENT_TYPE_SIGS:
                    if (th->sigpendcnt > 0)
                    {
                        lll_internal_lock(&th->siglock);
                        for (int sig = 1; sig < APTH_NSIG; sig++)
                        {
                            if (sigismember(event->ev_args.SIGS.sigs, sig) &&
                                sigismember(&th->sigpending, sig))
                            {
                                if (event->ev_args.SIGS.sig)
                                    *(event->ev_args.SIGS.sig) = sig;
                                sigdelset(&th->sigpending, sig);
                                th->sigpendcnt--;
                                event->ev_status = APTH_EV_STATUS_OCCURRED;
                                any_occurred = true;
                                break;
                            }
                        }
                        lll_internal_unlock(&th->siglock);
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

        // Transfer waked threads from waiting to waked queue.
        // For threads with FD events, tell the reactor to remove pending FD registrations.
        do
        {
            for (int i = 0; i < wake_count; i++)
            {
                apth_t th = wake_batch[i];
                // Tell reactor to remove any remaining FD events for this thread
                FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
                {
                    apth_event_t event = apth_event_t_list_entry(ev_e);
                    if (event->ev_type == APTH_EVENT_TYPE_FD && event->epoll_registered)
                        apth_reactor_remove_waiter(sched, event->ev_args.FD.fd, th, event);
                }
                atomic_store_release(&th->state, APTH_STATE_WAKED);
                transfer_th(th, THQUEUE(sched, waiting), THQUEUE(sched, waked));
            }

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

        // If there are waked threads, return immediately to dispatch them
        if (notified_ths > 0)
            dopoll = true;

        // ==================== Phase 2: block on wake_eventfd ====================
        // The global reactor handles all FD epoll_wait. The scheduler only needs
        // to block on its wake_eventfd to be notified when:
        //   - The reactor completes an FD event for one of our threads
        //   - Another scheduler pushes work to our queues
        //   - A timer fires (via timeout)

        int timeout_ms;
        if (dopoll)
        {
            // We have ready work; don't block
            timeout_ms = -1;
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
                timeout_ms = 60000;
        }
        else
        {
            // No timer, no ready work: block briefly on wake_eventfd.
            timeout_ms = 10;
        }

        if (timeout_ms >= 0)
        {
            struct epoll_event ep_events[4];
            int nready = epoll_wait(sched->epoll_fd, ep_events, 4, timeout_ms);

            if (nready > 0)
            {
                // Drain the wake eventfd
                for (int i = 0; i < nready; i++)
                {
                    if (ep_events[i].data.fd == sched->wake_eventfd)
                    {
                        uint64_t val;
                        ssize_t __ignored = apth_func_raw(read)(sched->wake_eventfd, &val, sizeof(val));
                        (void)__ignored;
                    }
                }
            }
            else if (nready == 0 && !dopoll && has_timer)
            {
                // Timer might have fired
                if (nexttimer_ev != NULL)
                {
                    if (nexttimer_ev->ev_type == APTH_EVENT_TYPE_FUNC)
                    {
                        loop_repeat = true;
                    }
                    else
                    {
                        nexttimer_ev->ev_status = APTH_EV_STATUS_OCCURRED;
                        apth_debug("[timeout] event occurred for apth \"%s\"", nexttimer_th->name);
                        FOR_ELEMENT_IN_LIST(nexttimer_th->event_list, ev_e)
                        {
                            apth_event_t event = apth_event_t_list_entry(ev_e);
                            if (event->ev_type == APTH_EVENT_TYPE_FD && event->epoll_registered)
                                apth_reactor_remove_waiter(sched, event->ev_args.FD.fd, nexttimer_th, event);
                        }
                        atomic_store_release(&nexttimer_th->state, APTH_STATE_WAKED);
                        transfer_th(nexttimer_th, THQUEUE(sched, waiting), THQUEUE(sched, waked));
                    }
                }
            }
        }

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
    list_push_back(el, &ev->elem);
}

APTH_INTERNAL void apth_event_isolate(apth_event_t ev)
{
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
    self->event_list = *el;

    // // Eagerly register FD events with the global reactor before yielding.
    // {
    //     apth_sched_t sched = SCHED_OF(self);
    //     FOR_ELEMENT_IN_LIST(self->event_list, pre_e)
    //     {
    //         apth_event_t pre_ev = apth_event_t_list_entry(pre_e);
    //         if (pre_ev->ev_type == APTH_EVENT_TYPE_FD)
    //             apth_reactor_add_waiter(sched, pre_ev->ev_args.FD.fd, self, pre_ev);
    //     }
    // }

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

    // Eagerly register FD event with the global reactor
    if (ev->ev_type == APTH_EVENT_TYPE_FD)
        apth_reactor_add_waiter(SCHED_OF(self), ev->ev_args.FD.fd, self, ev);

    // Move thread into waiting state and transfer control to scheduler
    atomic_store_release(&self->state, APTH_STATE_WAITING);
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    // Check for cancellation
    apth_cancel_point();

    // Unlink event from current thread
    list_remove(&ev->elem);
    assert_msg(list_empty(&self->event_list), "insane");

    bool result = false;
    if (ev->ev_status != APTH_EV_STATUS_PENDING)
    {
        result = true;
    }

    apth_debug("leave to thread \"%s\"", self->name);
    return result;
}
