#include "common.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include <malloc.h>

static bool apth_state_matches_event_goal(apth_state_t state, apth_goal_t goal)
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
    if (th->cancelreq == true)
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
            // TODO
            break;
        case APTH_EVENT_TYPE_COND:
            // TODO
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
                        while ((rc2 = apth_syscall_raw(select)(event->ev_args.FD.fd + 1, &aux->rfds, &aux->wfds, &aux->efds, &aux->delay)) < 0 && errno == EINTR)
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
                        while ((rc2 = apth_syscall_raw(select)(event->ev_args.SELECT.nfd + 1, prfds, pwfds, pefds, &aux->delay)) < 0 && errno == EINTR)
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
                    // clean signal
                    // TODO: cond handle implementation
                }
            }
            // Local to global mapping
            if (event->ev_status != APTH_EV_STATUS_PENDING)
                any_occurred = true;
        }
    }

    // Cancellation support
    if (th->cancelreq == true)
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
                (aux.rc = apth_syscall_raw(select)(aux.fdmax + 1, &aux.rfds, &aux.wfds, &aux.efds, aux.pdelay)) < 0 && errno == EINTR)
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

APTH_INTERNAL apth_event_t apth_event_mutex(unsigned long spec /* TODO */)
{
    apth_event_t ev = prepare_ev(spec);
    TODO("apth_event_mutex");
    return ev;
}

APTH_INTERNAL apth_event_t apth_event_cond(unsigned long spec /* TODO */)
{
    apth_event_t ev = prepare_ev(spec);
    TODO("apth_event_cond");
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