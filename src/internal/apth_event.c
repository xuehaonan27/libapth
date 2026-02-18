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
    apth_debug("apth_sched_eventmanager: enter in %s mode", dopoll ? "polling" : "waiting");

    for (;;)
    {
        apth_debug("loop");
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

        // apth_debug("waiting list is empty? %s", list_empty(&sched->waiting_list) ? "true" : "false");

        FOR_ELEMENT_IN_LIST(sched->waiting_list, e)
        {
            apth_t th = apth_t_list_entry(e);
            apth_debug("checking in waiting list th=%p", th);

            // Determine signals we block
            // If there's any apth that do not block `sig`, then the worker pthread should
            // not block `sig`.
            for (int sig = 1; sig < APTH_NSIG; sig++)
            {
                if (!sigismember(&(th->ctx->sigs), sig))
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
                             apth_state_matches_event_goal(event->ev_args.TID.tid->state, event->ev_goal)))
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
        }
        else
        {
            // No haste, no event to be checked later.
            // Do a polling without a timeout, i.e. wait for the fd sets only with blocking.
            pdelay = NULL;
        }

        // Clear pipe and let select() wait for read-part of the pipe.
        // apth_debug("going to select wait for read-part of the pipe");
        char minibuf[128];
        // int tmp_mode = fcntl(sched->apth_sigpipe[0], F_GETFL, NULL);
        // apth_debug("sched->apth_sigpipe[0] is nonblocking?: %s", tmp_mode & APTH_O_NONBLOCKING ? "true" : "false");
        while (apth_syscall_raw(read)(sched->apth_sigpipe[0], minibuf, sizeof(minibuf)) > 0);
        FD_SET(sched->apth_sigpipe[0], &rfds);
        if (fdmax < sched->apth_sigpipe[0]) {
            apth_debug("before fdmax=%d, now should be %d", fdmax, sched->apth_sigpipe[0]);
            fdmax = sched->apth_sigpipe[0];
        }

        // Replace signal actions for signals we have to catch for events
        struct sigaction sa;
        struct sigaction osa[1 + APTH_NSIG];
        // apth_debug("replace signal actions for signals");
        for (int sig = 1; sig < APTH_NSIG; sig++)
        {
            if (sigismember(&sched->apth_sigcatch, sig))
            {
                // apth_debug("register sig=%d with apth_sched_eventmanager_sighandler", sig);
                sa.sa_sigaction = apth_sched_eventmanager_sighandler;
                sigfillset(&sa.sa_mask);
                sa.sa_flags = SA_SIGINFO;
                sigaction(sig, &sa, &osa[sig]);
            }
        }
        // apth_debug("replaced");

        // Allow some signals to be delivered: either to our catching handler or directly
        // to the configured handler for signals not catched by events
        sigset_t oss;
        apth_syscall_raw(pthread_sigmask)(SIG_SETMASK, &sched->apth_sigblock, &oss);

        // Now do the polling for filedescriptor I/O and timers.
        // When the scheduler sleeps at all, then here.
        // apth_debug("Now do the polling for fd I/O and timers");
        // apth_debug("dopoll=%s, fdmax=%d", dopoll ? "true" : "false", fdmax);
        // apth_debug("pdelay=%p", pdelay);
        // int tmp_mode = fcntl(sched->apth_sigpipe[0], F_GETFL, NULL);
        // apth_debug("sched->apth_sigpipe[0] is nonblocking?: %s", tmp_mode & APTH_O_NONBLOCKING ? "true" : "false");
        int rc = -1;
        if (!(dopoll && fdmax == -1))
            // !dopoll: then there's some events occured.
            // !fdmax == -1, then there's some fd to wait for.
            while (
                (rc = apth_syscall_raw(select)(fdmax + 1, &rfds, &wfds, &efds, pdelay)) < 0 && errno == EINTR)
                {
                    apth_debug("rc=%d, errno=%d", rc, errno);
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
                // apth_debug("loop_repeat");
                loop_repeat = true;
            }
            else
            {
                // It was an explicit timer event, standing for its own.
                apth_debug("apth_sched_eventmanager: [timeout] event occurred for apth \"%s\"", nexttimer_th->name);
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
        FOR_ELEMENT_IN_LIST(sched->waiting_list, wth_e)
        {
            // Move last apth with events occurred to ready queue.
            // Insert it with a slightly increased queue priority.
            if (th_last != APTH_NULL)
            {
                // TODO: lock the list
                // list_remove(&th_last->elem);
                remove_apth(th_last);
                th_last->state = APTH_STATE_READY;
                // TODO: give th_last a higher prio
                push_apth_to_ready(th_last, sched);
                apth_debug("apth_sched_eventmanager: apth \"%s\" moved from waiting to ready queue", th_last->name);
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
                                apth_debug("apth_sched_eventmanager: [I/O event occurred for thread \"%s\"]", th->name);
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
                                    apth_debug("apth_sched_eventmanager: [I/O] event failed for thread \"%s\"",
                                               th->name);
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
                                apth_debug("pth_sched_eventmanager: [I/O] event occurred for thread \"%s\"",
                                           th->name);
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
                                    apth_debug("pth_sched_eventmanager: [I/O] event failed for thread \"%s\"",
                                               th->name);
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
                                        apth_debug("apth_sched_eventmanager: [signal] event occurred for apth \"%s\"", th->name);
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
                apth_debug("apth_sched_eventmanager: cancellation request pending for apth \"%s\"", th->name);
                any_occurred = true;
            }

            if (any_occurred)
            {
                th_last = th;
            }
        }

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
    self->state = APTH_STATE_WAITING;
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
    apth_debug("apth_wait_events: enter from thread \"%s\"", self->name);

    // Mark the event as still pending
    ev->ev_status = APTH_EV_STATUS_PENDING;

    // Link event into current thread
    apth_event_list_add(&self->event_list, ev);

    // Move thread into waiting state and transfer control to scheduler
    self->state = APTH_STATE_WAITING;
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

    // Leave to current thread with number of occurred events
    apth_debug("apth_wait_event_list: leave to thread \"%s\"", self->name);
    return result;
}