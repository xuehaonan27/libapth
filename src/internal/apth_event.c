#include "common.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include <malloc.h>

static void apth_sched_eventmanager_sighandler(int sig, MAYBE_UNUSED siginfo_t *_dummy_info, void *arg)
{
    char c;
    apth_sched_t sched = (struct apth_perpthr_scheduler *)arg;
    // Remember raised signal
    sigaddset(&sched->apth_sigraised, sig);

    // Write signal to signal pipe in order to awake the select()
    c = (int)sig;
    apth_debug("GOING TO WRITE TO PIPE");
    apth_syscall_raw(write)(sched->apth_sigpipe[1], &c, sizeof(char));
    return;
}

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

// Look whether some events already occurred (or failed) and move corresponding
// apthes from waiting queue back to ready queue
APTH_INTERNAL void apth_sched_eventmanager(apth_sched_t sched, apth_time_t *now, bool dopoll)
{
    apth_debug("(%d) enter in %s mode", sched->id, dopoll ? "polling" : "waiting");

    for (;;)
    {
        // apth_debug("(%d) loop", sched->id);
        bool loop_repeat = false;

        // Initialize fd sets, for `select`
        fd_set rfds;
        fd_set wfds;
        fd_set efds;
        int fdmax;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        fdmax = -1;

        // Initialize signal status
        sigpending(&sched->apth_sigpending);
        sigfillset(&sched->apth_sigblock);
        sigemptyset(&sched->apth_sigcatch);
        sigemptyset(&sched->apth_sigraised);

        // Initialize next timer
        apth_t nexttimer_th;
        apth_event_t nexttimer_ev;
        apth_time_t nexttimer_value;
        apth_time_set(&nexttimer_value, APTH_TIME_ZERO);
        nexttimer_th = APTH_NULL;
        nexttimer_ev = APTH_EVENT_NULL;

        // For all apths in the waiting queue
        bool any_occurred = false;

        // lll_lock(&sched->waiting_list_lock, "event checking waiting list");
        FOR_ELEMENT_IN_LIST(sched->waiting_list, e)
        {
            apth_t th = apth_t_list_entry(e);
            apth_debug("checking in waiting list th=%p", th);

            // Determine signals we block
            // If there's any apth that do not block `sig`, then the worker pthread should
            // not block `sig`.
            for (int sig = 1; sig < APTH_NSIG; sig++)
            {
                // NOTE: check signal mask here
                sigset_t *th_signal_mask = &CTX_SIGMASK_OF(th->ctx);
#ifdef APTH_DEBUG
                // sigset_t th_signal_mask_acquired_by_libccall;
                // According to manual:
                // If  set  is  NULL,  then the signal mask is unchanged (i.e., how is ignored),
                // but the current value of the signal mask is nevertheless returned in oldset
                // (if it is not NULL).

                // apth_syscall_raw(pthread_sigmask)(SIG_SETMASK, NULL, &th_signal_mask_acquired_by_libccall);

                // for (int sig = 1; sig < APTH_NSIG; sig++)
                // {
                //     if (sigismember(th_signal_mask, sig) &&
                //         !sigismember(&th_signal_mask_acquired_by_libccall, sig))
                //         apth_debug("sig = %d in th_signal_mask but not...", sig);
                //     else if (
                //         !sigismember(th_signal_mask, sig) &&
                //         sigismember(&th_signal_mask_acquired_by_libccall, sig))
                //         apth_debug("sig = %d in th_signal_mask_acquired_by_libccall but not...", sig);
                // }

                // apth_debug("th_signal_mask = %lx", *(uint64_t *)th_signal_mask);
                // apth_debug("th_signal_mask_acquired_by_libccall = %lx", *(uint64_t *)(&th_signal_mask_acquired_by_libccall));

                // assert(memcmp(th_signal_mask, &th_signal_mask_acquired_by_libccall, sizeof(sigset_t)) == 0);
#endif
                if (!sigismember(th_signal_mask, sig))
                    sigdelset(&sched->apth_sigblock, sig);
            }

            // Cancellation support
            if (th->cancelreq == true)
                any_occurred = true;

            // If this apth do not have any event, then go ahead
            if (list_empty(&th->event_list))
                continue;

            // There's events for this apth, check whether events occurred.
            bool this_ev_occurred = false;
            FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
            {
                apth_event_t event = apth_event_t_list_entry(ev_e);

                if (event->ev_status == APTH_EV_STATUS_PENDING)
                {
                    this_ev_occurred = false;

                    switch (event->ev_type)
                    {
                    case APTH_EVENT_TYPE_FD:
                        // Filedescriptor I/O
                        // Filedescriptors are checked later all at once. Here we
                        // only assemble them in the fd sets
                        if (event->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
                            FD_SET(event->ev_args.FD.fd, &rfds);
                        if (event->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
                            FD_SET(event->ev_args.FD.fd, &wfds);
                        if (event->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
                            FD_SET(event->ev_args.FD.fd, &efds);
                        if (fdmax < event->ev_args.FD.fd)
                            fdmax = event->ev_args.FD.fd;
                        break;
                    case APTH_EVENT_TYPE_SELECT:
                        // Filedescriptor Set Select I/O
                        // Filedescriptors are checked later all at once. Here we
                        // only assemble them in the fd sets
                        apth_util_fds_merge(event->ev_args.SELECT.nfd,
                                            event->ev_args.SELECT.rfds, &rfds,
                                            event->ev_args.SELECT.wfds, &wfds,
                                            event->ev_args.SELECT.efds, &efds);
                        if (fdmax < event->ev_args.SELECT.nfd - 1)
                            fdmax = event->ev_args.SELECT.nfd - 1;
                        break;
                    case APTH_EVENT_TYPE_SIGS:
                        // Signal Set
                        for (int sig = 1; sig < APTH_NSIG; sig++)
                        {
                            if (sigismember(event->ev_args.SIGS.sigs, sig))
                            {
                                // Apth signal handling
                                if (sigismember(&th->sigpending, sig))
                                {
                                    // This signal is both in event goal and the apth
                                    // so move the pending signal from apth to event
                                    *(event->ev_args.SIGS.sig) = sig;
                                    sigdelset(&th->sigpending, sig);
                                    th->sigpendcnt--;
                                    this_ev_occurred = true;
                                }

                                // Pthread signal handling
                                if (sigismember(&sched->apth_sigpending, sig))
                                {
                                    // This signal is both in event goal and pthread
                                    // so move the pending signal from pthread to event
                                    if (event->ev_args.SIGS.sig != NULL)
                                        *(event->ev_args.SIGS.sig) = sig;
                                    apth_util_sigdelete(sig);
                                    this_ev_occurred = true;
                                }
                                else
                                {
                                    // This signal is in event goal but not in pthread
                                    // pending set. So allow the signal, and add it to
                                    // catch set.
                                    sigdelset(&sched->apth_sigblock, sig);
                                    sigaddset(&sched->apth_sigcatch, sig);
                                }
                            }
                        }
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
                                (nexttimer_th == NULL && nexttimer_ev == NULL) ||
                                (apth_time_cmp(&(event->ev_args.TIME.tv), &nexttimer_value) < 0))
                            {
                                nexttimer_th = th;
                                nexttimer_ev = event;
                                apth_time_set(&nexttimer_value, &(event->ev_args.TIME.tv));
                            }
                        }
                        break;
                    case APTH_EVENT_TYPE_MUTEX:
                        break;
                    case APTH_EVENT_TYPE_COND:
                        break;
                    case APTH_EVENT_TYPE_TID:
                        // Thread termination
                        if ((event->ev_args.TID.tid == NULL && !list_empty(&sched->terminated_list)) ||
                            (event->ev_args.TID.tid != NULL &&
                             apth_state_matches_event_goal(raw_state_of(event->ev_args.TID.tid), event->ev_goal)))
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
                            if ((nexttimer_th == NULL && nexttimer_ev == NULL) ||
                                apth_time_cmp(&tv, &nexttimer_value) < 0)
                            {
                                nexttimer_th = th;
                                nexttimer_ev = event;
                                apth_time_set(&nexttimer_value, &tv);
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
                        any_occurred = true;
                    }
                }
            }
        }
        // lll_unlock(&sched->waiting_list_lock, "event checking waiting list");

        // apth_debug("any_occurred = %s", any_occurred ? "true" : "false");
        // If any event occurred, then we should do poll mode for select
        if (any_occurred)
            dopoll = true;

        // Now decide how to poll for fd I/O and timers
        struct timeval delay;
        struct timeval *pdelay;

        if (dopoll)
        {
            // When some event occurred or there's more apths to run, then we should do this,
            // instead of wasting more time on polling events and I/O s.
            // Do a polling with immediate timeout, i.e. check the fd sets only without blocking
            apth_time_set(&delay, APTH_TIME_ZERO);
            pdelay = &delay;
        }
        else if (nexttimer_ev != NULL)
        {
            // There's an event needed to be checked later.
            // Do a polling with a timeout set to the next timer, i.e. wait for the fd sets or
            // the next timer.
            apth_time_set(&delay, &nexttimer_value);
            apth_time_sub(&delay, now);
            pdelay = &delay;

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
            // Pth, we decide that not to wait, but notify the scheduler: GO TO STEAL
            apth_time_set(&delay, APTH_TIME_ZERO);
            pdelay = &delay;
            // TODO: mark to notify this scheduler to steal work
        }

        // Clear pipe and let select() wait for read-part of the pipe.
        // apth_debug("(%d) GOING TO read the read-part of the signal pipe", sched->id);
        assert(fcntl(sched->apth_sigpipe[0], F_GETFL, NULL) == APTH_O_NONBLOCKING);
        char minibuf[128];
        while (apth_syscall_raw(read)(sched->apth_sigpipe[0], minibuf, sizeof(minibuf)) > 0)
            ;
        // apth_debug("(%d) HAS read the read-part of the signal pipe", sched->id);
        // FD_SET(sched->apth_sigpipe[0], &rfds);
        // if (fdmax < sched->apth_sigpipe[0])
        // {
        //     apth_debug("(%d) before fdmax=%d, now should be %d", sched->id, fdmax, sched->apth_sigpipe[0]);
        //     fdmax = sched->apth_sigpipe[0];
        // }

        // Replace signal actions for signals we have to catch for events
        struct sigaction sa;
        struct sigaction osa[1 + APTH_NSIG];
        bool at_least_one_signal_to_be_catched = false;
        for (int sig = 1; sig < APTH_NSIG; sig++)
        {
            if (sigismember(&sched->apth_sigcatch, sig))
            {
                apth_debug("(%d) sig=%d is in my `apth_sigcatch`", sched->id, sig);
                sa.sa_sigaction = apth_sched_eventmanager_sighandler;
                sigfillset(&sa.sa_mask);
                sa.sa_flags = SA_SIGINFO;
                sigaction(sig, &sa, &osa[sig]);
                at_least_one_signal_to_be_catched = true;
            }
        }

        // If there's at least one signal to catch
        if (at_least_one_signal_to_be_catched)
        {
            FD_SET(sched->apth_sigpipe[0], &rfds);
            if (fdmax < sched->apth_sigpipe[0])
            {
                apth_debug("(%d) before fdmax=%d, now should be %d", sched->id, fdmax, sched->apth_sigpipe[0]);
                fdmax = sched->apth_sigpipe[0];
            }
        }

        // Allow some signals to be delivered: either to our catching handler or directly
        // to the configured handler for signals not catched by events
        sigset_t oss;
        apth_syscall_raw(pthread_sigmask)(SIG_SETMASK, &sched->apth_sigblock, &oss);

        // Now do the polling for filedescriptor I/O and timers.
        // When the scheduler sleeps at all, then here.
        int rc = -1;
        // if (!(dopoll && fdmax == -1))

        // TODO: Firstly, if scheduler has more apth to schedule, scheduler would not wait here
        // TODO: Then, scheduler should check whether work stealing is available.
        // Then, if the scheduler really really has nothing to do, then could the sched
        // goes here:
        //      !fdmax == -1: no more work to haste, and there's some fd to wait for.
        //      !dopoll: no more work to haste, and there is a time event
        if (fdmax != -1 || !dopoll)
        {
            apth_debug("(%d) GOING TO select the fd_set, fdmax=%d", sched->id, fdmax);
            while (
                (rc = apth_syscall_raw(select)(fdmax + 1, &rfds, &wfds, &efds, pdelay)) < 0 && errno == EINTR)
            {
                apth_debug("(%d) rc=%d, errno=%d", sched->id, rc, errno);
            }
            // apth_debug("(%d) HAS select the fd_set", sched->id);
        }

        // Restore signal mask and actions and handle signals
        // apth_debug("Restore signal mask and actions and handle signals");
        apth_syscall_raw(pthread_sigmask)(SIG_SETMASK, &oss, NULL);
        for (int sig = 1; sig < APTH_NSIG; sig++)
        {
            if (sigismember(&sched->apth_sigcatch, sig))
                sigaction(sig, &osa[sig], NULL);
        }

        // If the timer elapsed, handle it
        if (!dopoll && rc == 0 && nexttimer_ev != NULL)
        {
            if (nexttimer_ev->ev_type == APTH_EVENT_TYPE_FUNC)
            {
                // It was an implicit timer event for a function event.
                // So repeat the event handling for rechecking the function
                loop_repeat = true;
            }
            else
            {
                // It was an explicit timer event, standing for its own.
                apth_debug("(%d) [timeout] event occurred for apth \"%s\"",
                           sched->id, nexttimer_th->name);
                nexttimer_ev->ev_status = APTH_EV_STATUS_OCCURRED;
            }
        }

        // If the internal signal pipe was used, adjust the select() results
        if (!dopoll && rc > 0 && FD_ISSET(sched->apth_sigpipe[0], &rfds))
        {
            FD_CLR(sched->apth_sigpipe[0], &rfds);
            rc--;
        }

        // If an error occurred, avoid confusion in the cleaup loop
        if (rc <= 0)
        {
            FD_ZERO(&rfds);
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
        }

        // Final cleanup loop where we have to do 2 jobs:
        // 1. Late handling of the fd I/O events
        // 2. If an apth has one occurred event, we move it from the waiting queue to the ready queue

        // For all apths in the waiting queue
        apth_t th_last = APTH_NULL;
        // lll_lock(&sched->waiting_list_lock, "event checking waiting list");
        FOR_ELEMENT_IN_LIST(sched->waiting_list, wth_e)
        {
            // Move last apth with events occurred to ready queue.
            // Insert it with a slightly increased queue priority.
            if (th_last != APTH_NULL)
            {
                remove_apth(th_last);
                // th_last->state = APTH_STATE_READY;
                // We want to submit READY state to `th_last`
                submit_desired_state_to(th_last, APTH_STATE_WAKED);
                // TODO: give th_last a higher prio
                push_apth_to_waked(th_last, sched);
                apth_debug("(%d) apth \"%s\" moved from waiting to ready queue", sched->id, th_last->name);
                th_last = APTH_NULL;
            }

            apth_t th = apth_t_list_entry(wth_e);

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
                                FD_ISSET(event->ev_args.FD.fd, &rfds);
                            bool write_condition =
                                (event->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE) &&
                                FD_ISSET(event->ev_args.FD.fd, &wfds);
                            bool excep_condition =
                                (event->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION) &&
                                FD_ISSET(event->ev_args.FD.fd, &efds);
                            if (read_condition ||
                                write_condition ||
                                excep_condition)
                            {
                                apth_debug("(%d) [I/O event occurred for thread \"%s\"]", sched->id, th->name);
                                event->ev_status = APTH_EV_STATUS_OCCURRED;
                            }
                            else if (rc < 0)
                            {
                                // re-check particular filedescriptor
                                int rc2;

                                if (event->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
                                    FD_SET(event->ev_args.FD.fd, &rfds);
                                if (event->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
                                    FD_SET(event->ev_args.FD.fd, &wfds);
                                if (event->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
                                    FD_SET(event->ev_args.FD.fd, &efds);

                                apth_time_set(&delay, APTH_TIME_ZERO);
                                while ((rc2 = apth_syscall_raw(select)(event->ev_args.FD.fd + 1, &rfds, &wfds, &efds, &delay)) < 0 && errno == EINTR)
                                    ;

                                if (rc2 > 0)
                                {
                                    // cleanup afterwards for next iteration
                                    FD_CLR(event->ev_args.FD.fd, &rfds);
                                    FD_CLR(event->ev_args.FD.fd, &wfds);
                                    FD_CLR(event->ev_args.FD.fd, &efds);
                                }
                                else if (rc2 < 0)
                                {
                                    // cleanup afterwards for next iteration
                                    FD_ZERO(&rfds);
                                    FD_ZERO(&wfds);
                                    FD_ZERO(&efds);
                                    event->ev_status = APTH_EV_STATUS_FAILED;
                                    apth_debug("(%d) [I/O] event failed for thread \"%s\"", sched->id, th->name);
                                }
                            }
                            break;
                        case APTH_EVENT_TYPE_SELECT:
                            // Filedescriptor Set I/O
                            if (apth_util_fds_test(event->ev_args.SELECT.nfd,
                                                   event->ev_args.SELECT.rfds, &rfds,
                                                   event->ev_args.SELECT.wfds, &wfds,
                                                   event->ev_args.SELECT.efds, &efds))
                            {
                                int n = apth_util_fds_select(event->ev_args.SELECT.nfd,
                                                             event->ev_args.SELECT.rfds, &rfds,
                                                             event->ev_args.SELECT.wfds, &wfds,
                                                             event->ev_args.SELECT.efds, &efds);
                                if (event->ev_args.SELECT.n != NULL)
                                    *(event->ev_args.SELECT.n) = n;
                                event->ev_status = APTH_EV_STATUS_OCCURRED;
                                apth_debug("(%d) [I/O] event occurred for thread \"%s\"", sched->id, th->name);
                            }
                            else if (rc < 0)
                            {
                                /* re-check particular filedescriptor set */
                                int rc2;
                                fd_set *prfds = NULL;
                                fd_set *pwfds = NULL;
                                fd_set *pefds = NULL;
                                fd_set trfds;
                                fd_set twfds;
                                fd_set tefds;
                                if (event->ev_args.SELECT.rfds)
                                {
                                    memcpy(&trfds, event->ev_args.SELECT.rfds, sizeof(rfds));
                                    prfds = &trfds;
                                }
                                if (event->ev_args.SELECT.wfds)
                                {
                                    memcpy(&twfds, event->ev_args.SELECT.wfds, sizeof(wfds));
                                    pwfds = &twfds;
                                }
                                if (event->ev_args.SELECT.efds)
                                {
                                    memcpy(&tefds, event->ev_args.SELECT.efds, sizeof(efds));
                                    pefds = &tefds;
                                }
                                apth_time_set(&delay, APTH_TIME_ZERO);
                                while ((rc2 = apth_syscall_raw(select)(event->ev_args.SELECT.nfd + 1, prfds, pwfds, pefds, &delay)) < 0 && errno == EINTR)
                                    ;
                                if (rc2 < 0)
                                {
                                    event->ev_status = APTH_EV_STATUS_FAILED;
                                    apth_debug("(%d) [I/O] event failed for thread \"%s\"", sched->id, th->name);
                                }
                            }
                            break;
                        case APTH_EVENT_TYPE_SIGS:
                            for (int sig = 1; sig < APTH_NSIG; sig++)
                            {
                                if (sigismember(event->ev_args.SIGS.sigs, sig))
                                {
                                    if (sigismember(&sched->apth_sigraised, sig))
                                    {
                                        // If sig is in both event and this pthread raised signals
                                        if (event->ev_args.SIGS.sig != NULL)
                                            *(event->ev_args.SIGS.sig) = sig;
                                        apth_debug("(%d) [signal] event occurred for apth \"%s\"", sched->id, th->name);
                                        sigdelset(&sched->apth_sigraised, sig);
                                        event->ev_status = APTH_EV_STATUS_OCCURRED;
                                    }
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
                apth_debug("(%d) cancellation request pending for apth \"%s\"", sched->id, th->name);
                any_occurred = true;
            }

            if (any_occurred)
            {
                th_last = th;
            }
        }
        // lll_unlock(&sched->waiting_list_lock, "event checking waiting list");

        // If the last th is also someone with occurred event, move it to ready queue as well
        if (th_last != APTH_NULL)
        {
            remove_apth(th_last);
            // th_last->state = APTH_STATE_READY;
            // We want to submit READY state to `th_last`
            submit_desired_state_to(th_last, APTH_STATE_WAKED);
            // TODO: give th_last a higher prio
            push_apth_to_waked(th_last, sched);
            apth_debug("(%d) apth \"%s\" moved from waiting to ready queue", sched->id, th_last->name);
            th_last = APTH_NULL;
        }

        if (loop_repeat)
        {
            apth_debug("(%d) continue", sched->id);
            apth_time_set(now, APTH_TIME_NOW);
            continue;
        }
        else
        {
            apth_debug("(%d) leave", sched->id);
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
    submit_desired_state_to(self, APTH_STATE_WAITING);
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
    apth_debug("(%d) enter from thread \"%s\"", sched_of(self)->id, self->name);

    // Mark the event as still pending
    ev->ev_status = APTH_EV_STATUS_PENDING;

    // Link event into current thread
    apth_event_list_add(&self->event_list, ev);

    // Move thread into waiting state and transfer control to scheduler
    // self->state = APTH_STATE_WAITING;
    // We want to submit WAITING state to `self`
    submit_desired_state_to(self, APTH_STATE_WAITING);
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
    apth_debug("(%d) leave to thread \"%s\"", sched_of(self)->id, self->name);
    return result;
}