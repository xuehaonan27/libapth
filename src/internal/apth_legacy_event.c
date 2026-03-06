#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/atomic_wrapper.h"

struct aux_for_eventmanager
{
    apth_sched_t sched;
    apth_time_t *now;

    fd_set rfds;
    fd_set wfds;
    fd_set efds;
    int fdmax;

    apth_t nexttimer_th;
    apth_event_t nexttimer_ev;
    apth_time_t nexttimer_value;

    struct timeval delay;
    struct timeval *pdelay;

    int rc; // int rc = -1;
};

static apth_thqueue_t __first_loop(apth_t th, void *aux_arg)
{
    apth_debug("checking in waiting list th=%p", th);

    struct aux_for_eventmanager *aux = (struct aux_for_eventmanager *)aux_arg;
    apth_sched_t sched = aux->sched;
    apth_time_t *now = aux->now;

    apth_thqueue_t ret_val = NULL;

    // Cancellation support
    if (atomic_load_acquire(&th->cancelreq))
        ret_val = APTH_DONT_MOVE_BUT_COUNT;

    // If this apth do not have any event, then go ahead
    if (list_empty(&th->event_list))
        return ret_val;

    // There's events for this apth, check whether events occurred.
    bool this_ev_occurred = false;
    FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
    {
        apth_event_t event = apth_event_t_list_entry(ev_e);

        if (event->ev_status != APTH_EV_STATUS_PENDING)
            continue;

        this_ev_occurred = false;

        switch (event->ev_type)
        {
        case APTH_EVENT_TYPE_FD:
            // Filedescriptor I/O
            // Filedescriptors are checked later all at once. Here we
            // only assemble them in the fd sets
            if (event->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
                FD_SET(event->ev_args.FD.fd, &aux->rfds);
            if (event->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
                FD_SET(event->ev_args.FD.fd, &aux->wfds);
            if (event->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
                FD_SET(event->ev_args.FD.fd, &aux->efds);
            if (aux->fdmax < event->ev_args.FD.fd)
                aux->fdmax = event->ev_args.FD.fd;
            break;
        case APTH_EVENT_TYPE_SELECT:
            // Filedescriptor Set Select I/O
            // Filedescriptors are checked later all at once. Here we
            // only assemble them in the fd sets
            apth_util_fds_merge(event->ev_args.SELECT.nfd,
                                event->ev_args.SELECT.rfds, &aux->rfds,
                                event->ev_args.SELECT.wfds, &aux->wfds,
                                event->ev_args.SELECT.efds, &aux->efds);
            if (aux->fdmax < event->ev_args.SELECT.nfd - 1)
                aux->fdmax = event->ev_args.SELECT.nfd - 1;
            break;
        case APTH_EVENT_TYPE_SIGS:
            // Check apth level sigpending only, instead of pthread level
            for (int sig = 1; sig < APTH_NSIG; sig++)
            {
                if (sigismember(event->ev_args.SIGS.sigs, sig))
                {
                    lll_lock(&th->siglock, "event_sigs");
                    if (sigismember(&th->sigpending, sig))
                    {
                        // Signal matches, remove from pending and mark event as occurred
                        if (event->ev_args.SIGS.sig != NULL)
                            *(event->ev_args.SIGS.sig) = sig;
                        sigdelset(&th->sigpending, sig);
                        th->sigpendcnt--;
                        lll_unlock(&th->siglock, "event_sigs");
                        this_ev_occurred = true;
                        break; // only one signal matching is enough
                    }
                    lll_unlock(&th->siglock, "event_sigs");
                }
            }
            // sched->apth_sigblock / apth_sigcatch no longer needed
            break;
        case APTH_EVENT_TYPE_TIME:
            // Timer
            if (apth_time_cmp(&(event->ev_args.TIME.tv), now) < 0)
                // timed out
                this_ev_occurred = true;

            else
            {
                // Remember the timer which will be elapsed next
                if (
                    (aux->nexttimer_th == NULL && aux->nexttimer_ev == NULL) ||
                    (apth_time_cmp(&(event->ev_args.TIME.tv), &aux->nexttimer_value) < 0))
                {
                    aux->nexttimer_th = th;
                    aux->nexttimer_ev = event;
                    apth_time_set(&aux->nexttimer_value, &(event->ev_args.TIME.tv));
                }
            }
            break;
        case APTH_EVENT_TYPE_MUTEX:
            // No polling needed; apth_mutex_unlock directly marks event as OCCURRED.
            break;
        case APTH_EVENT_TYPE_COND:
            // No polling needed; apth_cond_signal/broadcast directly marks event as OCCURRED.
            break;
        case APTH_EVENT_TYPE_TID:
            // Thread termination
            if ((event->ev_args.TID.tid == NULL && thqueue_size(sched->terminated_queue) != 0) ||
                (event->ev_args.TID.tid != NULL &&
                 apth_state_matches_event_goal(state_holder_of(event->ev_args.TID.tid), event->ev_goal)))
                this_ev_occurred = true;
            break;
        case APTH_EVENT_TYPE_FUNC:
            if (event->ev_args.FUNC.func(event->ev_args.FUNC.arg))
                // Function returns true, so occurred
                this_ev_occurred = true;
            else
            {
                // Else we elapse for some time and check it again
                apth_time_t tv;
                apth_time_set(&tv, now);
                apth_time_add(&tv, &(event->ev_args.FUNC.tv));
                if ((aux->nexttimer_th == NULL && aux->nexttimer_ev == NULL) ||
                    apth_time_cmp(&tv, &aux->nexttimer_value) < 0)
                {
                    aux->nexttimer_th = th;
                    aux->nexttimer_ev = event;
                    apth_time_set(&aux->nexttimer_value, &tv);
                }
            }
            break;
        default:
            PANIC("Should not reach here");
            break;
        }

        // Tag this event if it has occurred
        if (this_ev_occurred)
        {
            apth_debug("apth_sched_eventmanager: [non-I/O] event occurred for apth \"%s\"", th->name);
            event->ev_status = APTH_EV_STATUS_OCCURRED;
            ret_val = APTH_DONT_MOVE_BUT_COUNT;
        }
    }

    return ret_val;
}

static apth_thqueue_t __second_loop(apth_t th, void *aux_arg)
{
    struct aux_for_eventmanager *aux = (struct aux_for_eventmanager *)aux_arg;
    apth_sched_t sched = aux->sched;

    // Do the late handling of the fd I/O and signal events in the waiting event
    bool any_occurred = false;

    if (!list_empty(&th->event_list))
    {
        FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
        {
            apth_event_t event = apth_event_t_list_entry(ev_e);

            // Late handling for still not occurred events
            if (event->ev_status == APTH_EV_STATUS_PENDING)
            {
                switch (event->ev_type)
                {
                case APTH_EVENT_TYPE_FD:
                {
                    bool read_condition =
                        (event->ev_goal & APTH_GOAL_UNTIL_FD_READABLE) &&
                        FD_ISSET(event->ev_args.FD.fd, &aux->rfds);
                    bool write_condition =
                        (event->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE) &&
                        FD_ISSET(event->ev_args.FD.fd, &aux->wfds);
                    bool excep_condition =
                        (event->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION) &&
                        FD_ISSET(event->ev_args.FD.fd, &aux->efds);
                    if (read_condition ||
                        write_condition ||
                        excep_condition)
                    {
                        apth_debug("[I/O event occurred for thread \"%s\"]", th->name);
                        event->ev_status = APTH_EV_STATUS_OCCURRED;
                    }
                    else if (aux->rc < 0)
                    {
                        // re-check particular filedescriptor
                        int rc2;

                        if (event->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
                            FD_SET(event->ev_args.FD.fd, &aux->rfds);
                        if (event->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
                            FD_SET(event->ev_args.FD.fd, &aux->wfds);
                        if (event->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
                            FD_SET(event->ev_args.FD.fd, &aux->efds);

                        apth_time_set(&aux->delay, APTH_TIME_ZERO);
                        while ((rc2 = apth_func_raw(select)(event->ev_args.FD.fd + 1, &aux->rfds, &aux->wfds, &aux->efds, &aux->delay)) < 0 && errno == EINTR)
                            ;

                        if (rc2 > 0)
                        {
                            // cleanup afterwards for next iteration
                            FD_CLR(event->ev_args.FD.fd, &aux->rfds);
                            FD_CLR(event->ev_args.FD.fd, &aux->wfds);
                            FD_CLR(event->ev_args.FD.fd, &aux->efds);
                        }
                        else if (rc2 < 0)
                        {
                            // cleanup afterwards for next iteration
                            FD_ZERO(&aux->rfds);
                            FD_ZERO(&aux->wfds);
                            FD_ZERO(&aux->efds);
                            event->ev_status = APTH_EV_STATUS_FAILED;
                            apth_debug("[I/O] event failed for thread \"%s\"", th->name);
                        }
                    }
                }
                break;
                case APTH_EVENT_TYPE_SELECT:
                    // Filedescriptor Set I/O
                    if (apth_util_fds_test(event->ev_args.SELECT.nfd,
                                           event->ev_args.SELECT.rfds, &aux->rfds,
                                           event->ev_args.SELECT.wfds, &aux->wfds,
                                           event->ev_args.SELECT.efds, &aux->efds))
                    {
                        int n = apth_util_fds_select(event->ev_args.SELECT.nfd,
                                                     event->ev_args.SELECT.rfds, &aux->rfds,
                                                     event->ev_args.SELECT.wfds, &aux->wfds,
                                                     event->ev_args.SELECT.efds, &aux->efds);
                        if (event->ev_args.SELECT.n != NULL)
                            *(event->ev_args.SELECT.n) = n;
                        event->ev_status = APTH_EV_STATUS_OCCURRED;
                        apth_debug("[I/O] event occurred for thread \"%s\"", th->name);
                    }
                    else if (aux->rc < 0)
                    {
                        // re-check particular filedescriptor set
                        int rc2;
                        fd_set *prfds = NULL;
                        fd_set *pwfds = NULL;
                        fd_set *pefds = NULL;
                        fd_set trfds;
                        fd_set twfds;
                        fd_set tefds;
                        if (event->ev_args.SELECT.rfds)
                        {
                            memcpy(&trfds, event->ev_args.SELECT.rfds, sizeof(aux->rfds));
                            prfds = &trfds;
                        }
                        if (event->ev_args.SELECT.wfds)
                        {
                            memcpy(&twfds, event->ev_args.SELECT.wfds, sizeof(aux->wfds));
                            pwfds = &twfds;
                        }
                        if (event->ev_args.SELECT.efds)
                        {
                            memcpy(&tefds, event->ev_args.SELECT.efds, sizeof(aux->efds));
                            pefds = &tefds;
                        }
                        apth_time_set(&aux->delay, APTH_TIME_ZERO);
                        while ((rc2 = apth_func_raw(select)(event->ev_args.SELECT.nfd + 1, prfds, pwfds, pefds, &aux->delay)) < 0 && errno == EINTR)
                            ;
                        if (rc2 < 0)
                        {
                            event->ev_status = APTH_EV_STATUS_FAILED;
                            apth_debug("[I/O] event failed for thread \"%s\"", th->name);
                        }
                    }
                    break;
                    // No longer check `sched->apth_sigraised`, because process
                    // level signals should already be delivered to certain
                    // apth's pending set by kernel level catch-all handler.
                case APTH_EVENT_TYPE_SIGS:
                    // Check apth's sigpending again (because there might be new signal arrives
                    // between __first_loop and __second_loop)
                    for (int sig = 1; sig < APTH_NSIG; sig++)
                    {
                        if (sigismember(event->ev_args.SIGS.sigs, sig))
                        {
                            lll_lock(&th->siglock, "event_sigs_2nd");
                            if (sigismember(&th->sigpending, sig))
                            {
                                if (event->ev_args.SIGS.sig != NULL)
                                    *(event->ev_args.SIGS.sig) = sig;
                                sigdelset(&th->sigpending, sig);
                                th->sigpendcnt--;
                                lll_unlock(&th->siglock, "event_sigs_2nd");
                                event->ev_status = APTH_EV_STATUS_OCCURRED;
                                break;
                            }
                            lll_unlock(&th->siglock, "event_sigs_2nd");
                        }
                    }
                    break;
                default:
                    break;
                }
            }
            // Post-processing for already occurred events
            else
            {
                if (event->ev_type == APTH_EVENT_TYPE_COND)
                {
                    // No-op: cond signal/broadcast marks event directly.
                }
            }
            // Local to global mapping
            if (event->ev_status != APTH_EV_STATUS_PENDING)
                any_occurred = true;
        }
    }

    // Cancellation support
    if (atomic_load_acquire(&th->cancelreq))
    {
        apth_debug("cancellation request pending for apth \"%s\"", th->name);
        any_occurred = true;
    }

    if (any_occurred)
        return sched->waked_queue;
    else
        return NULL;
}

// Look whether some events already occurred (or failed) and move corresponding
// apthes from waiting queue back to ready queue
APTH_INTERNAL void apth_sched_eventmanager(apth_sched_t sched, apth_time_t *now, bool dopoll)
{
    apth_debug("enter in %s mode", dopoll ? "polling" : "waiting");

    for (;;)
    {
        bool loop_repeat = false;

        struct aux_for_eventmanager aux = {
            .sched = sched,
            .now = now,
            .fdmax = -1,
            .rc = -1,
        };

        // Initialize fd sets, for `select`
        FD_ZERO(&aux.rfds);
        FD_ZERO(&aux.wfds);
        FD_ZERO(&aux.efds);

        // Initialize next timer
        apth_time_set(&aux.nexttimer_value, APTH_TIME_ZERO);
        aux.nexttimer_th = APTH_NULL;
        aux.nexttimer_ev = APTH_EVENT_NULL;

        // For all apths in the waiting queue
        size_t notified_ths = visit_thqueue(sched->waiting_queue, __first_loop, &aux);

        // If any event occurred, then we should do poll mode for select
        if (notified_ths > 0)
            dopoll = true;

        // Now decide how to poll for fd I/O and timers
        if (dopoll)
        {
            // When some event occurred or there's more apths to run, then we should do this,
            // instead of wasting more time on polling events and I/O s.
            // Do a polling with immediate timeout, i.e. check the fd sets only without blocking
            apth_time_set(&aux.delay, APTH_TIME_ZERO);
            aux.pdelay = &aux.delay;
        }
        else if (aux.nexttimer_ev != NULL)
        {
            // There's an event needed to be checked later.
            // Do a polling with a timeout set to the next timer, i.e. wait for the fd sets or
            // the next timer.
            apth_time_set(&aux.delay, &aux.nexttimer_value);
            apth_time_sub(&aux.delay, now);
            aux.pdelay = &aux.delay;

            // TODO: this is of low efficiency as well. Since time waiting for the timer to
            // expire could also be used to steal more work from other schedulers and run
            // them.
            // Moreover, if time interrupt is implemented, we could save this timer into the
            // scheduler, calculate how many time slices (floor it to integer ) needed for
            // the timer to expire, and enable time interrupt for this worker pthread
            // temporarily (if not explicitly enabled by programmer) and use the these piece
            // of time to do more work. For a very small timer (even not enough to fill up
            // one time slice), we could wait it here though.
        }
        else
        {
            // No haste, no event to be checked later.
            // Do a polling without a timeout, i.e. wait for the fd sets only with blocking.
            // pdelay = NULL;

            // NOTE: in a multithreaded environment, pausing indefinitely is of low efficiency.
            // Since this scheduler could steal work from other schedulers. So unlike in GNU
            // Pth, we decide not to wait, but notify the scheduler: GO AHEAD STEAL
            apth_time_set(&aux.delay, APTH_TIME_ZERO);
            aux.pdelay = &aux.delay;
            // TODO: mark to notify this scheduler to steal work
        }

        // TODO: Firstly, if scheduler has more apth to schedule, scheduler would not wait here
        // TODO: Then, scheduler should check whether work stealing is available.
        // Then, if the scheduler really really has nothing to do, then could the sched
        // goes here:
        //      !fdmax == -1: no more work to haste, and there's some fd to wait for.
        //      !dopoll: no more work to haste, and there is a time event
        if (aux.fdmax != -1 || !dopoll)
        {
            apth_debug("GOING TO select the fd_set, fdmax=%d", aux.fdmax);
            while (
                (aux.rc = apth_func_raw(select)(aux.fdmax + 1, &aux.rfds, &aux.wfds, &aux.efds, aux.pdelay)) < 0 && errno == EINTR)
            {
                apth_debug("rc=%d, errno=%d", aux.rc, errno);
            }
        }

        // If the timer elapsed, handle it
        if (!dopoll && aux.rc == 0 && aux.nexttimer_ev != NULL)
        {
            if (aux.nexttimer_ev->ev_type == APTH_EVENT_TYPE_FUNC)
            {
                // It was an implicit timer event for a function event.
                // So repeat the event handling for rechecking the function
                loop_repeat = true;
            }
            else
            {
                // It was an explicit timer event, standing for its own.
                apth_debug("[timeout] event occurred for apth \"%s\"", aux.nexttimer_th->name);
                aux.nexttimer_ev->ev_status = APTH_EV_STATUS_OCCURRED;
            }
        }

        // If an error occurred, avoid confusion in the cleaup loop
        if (aux.rc <= 0)
        {
            FD_ZERO(&aux.rfds);
            FD_ZERO(&aux.wfds);
            FD_ZERO(&aux.efds);
        }

        // Final cleanup loop where we have to do 2 jobs:
        // 1. Late handling of the fd I/O events
        // 2. If an apth has one occurred event, we move it from the waiting queue to the ready queue
        size_t _clean_loop_ret = visit_thqueue(sched->waiting_queue, __second_loop, &aux);
        (void)_clean_loop_ret; // we do not need it, so make compiler happy here

        if (loop_repeat)
        {
            apth_debug("continue");
            apth_time_set(now, APTH_TIME_NOW);
            continue;
        }
        else
        {
            apth_debug("leave");
            break;
        }
    }

    return;
}