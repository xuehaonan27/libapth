#include "hook_libc/hook_lowlevel_io.h"
#include "internal_types.h"
#include "internal_funcs.h"

APTH_DEFINE_HOOK(
    int, select,
    (int nfd, fd_set *rfds, fd_set *wfds, fd_set *efds, struct timeval *timeout),
    (nfd, rfds, wfds, efds, timeout))
{
    apth_event_t ev;
    apth_t cur = cur_apth();
    apth_debug("apth_func_select(hooked): called from thread \"%s\"", cur->name);

    // POSIX.1-2001/SUSv3 compliance
    if (nfd < 0 || nfd > FD_SETSIZE)
        return apth_error(-1, EINVAL);
    if (timeout != NULL)
    {
        // Check timeout sanity
        if (timeout->tv_sec < 0 || timeout->tv_usec < 0 || timeout->tv_usec >= 1000000)
            return apth_error(-1, EINVAL);
        // TODO: why set a month here
        if (timeout->tv_sec > 31 * 24 * 60 * 60) // 1 month
            timeout->tv_sec = 31 * 24 * 60 * 60;
    }

    // First deal with the special situation of a plain microsecond delay
    if (nfd == 0 && rfds == NULL && wfds == NULL && efds == NULL && timeout != NULL)
    {
        if (timeout->tv_sec == 0 && timeout->tv_usec < APTH_SELECT_DIRECT_TO_SCHED_THRESHOLD_US)
        {
            // Very small delays are acceptable to be performed directly
            while (apth_func_raw(select)(0, NULL, NULL, NULL, timeout) < 0 && errno == EINTR)
                ;
        }
        else
        {
            // Larger delays have to go through the scheduler
            ev = apth_event_time(APTH_EVENT_MODE_STATIC, apth_timeout(timeout->tv_sec, timeout->tv_usec));
            apth_wait_event(ev);
            apth_event_free(ev);
        }

        /*
        // POSIX.1-2001/SUSv3 compliance
        if (rfds != NULL)
            FD_ZERO(rfds);
        if (wfds != NULL)
            FD_ZERO(wfds);
        if (efds != NULL)
            FD_ZERO(efds);
        */
        return 0;
    }

    // Now directly poll filedescriptor sets to avoid unnecessary (and resource consuming
    // because of context switches, etc) event handling through the scheduler. We have to
    // be careful here, because not all platforms guarantee us that the sets are unmodified
    // if an error or timeout occurred. So we must prepare another set of them here.
    struct timeval delay;
    fd_set rspare, wspare, espare;
    fd_set *rtmp, *wtmp, *etmp;
    // int selected;
    int rc;

    delay.tv_sec = 0;
    delay.tv_usec = 0;
    rtmp = NULL;
    wtmp = NULL;
    etmp = NULL;
    if (rfds != NULL)
    {
        memcpy(&rspare, rfds, sizeof(fd_set));
        rtmp = &rspare;
    }
    if (wfds != NULL)
    {
        memcpy(&wspare, wfds, sizeof(fd_set));
        wtmp = &wspare;
    }
    if (efds != NULL)
    {
        memcpy(&espare, efds, sizeof(fd_set));
        etmp = &espare;
    }

    while ((rc = apth_func_raw(select)(nfd, rtmp, wtmp, etmp, &delay)) < 0 && errno == EINTR)
        ;
    if (rc < 0)
        // Pass-through immediate error
        return apth_error(-1, errno);
    else if (rc > 0 || (rc == 0 && timeout != NULL && apth_time_cmp(timeout, APTH_TIME_ZERO) == 0))
    {
        // Pass-through immediate success
        // Copy back results
        if (rfds != NULL)
            memcpy(rfds, &rspare, sizeof(fd_set));
        if (wfds != NULL)
            memcpy(wfds, &wspare, sizeof(fd_set));
        if (efds != NULL)
            memcpy(efds, &espare, sizeof(fd_set));
        return rc;
    }

    // Suspend currrent apth until one filedescriptor is ready or the timeout occurred.
    apth_event_t ev_select;
    apth_event_t ev_timeout;
    struct list event_list;
    list_init(&event_list);
    rc = -1;
    ev = ev_select = apth_event_select(APTH_EVENT_MODE_STATIC, &rc, nfd, rfds, wfds, efds);
    apth_event_list_add(&event_list, ev);
    ev_timeout = NULL;
    if (timeout != NULL)
    {
        ev_timeout = apth_event_time(APTH_EVENT_MODE_STATIC, apth_timeout(timeout->tv_sec, timeout->tv_usec));
        apth_event_list_add(&event_list, ev_timeout);
    }
    apth_wait_event_list(&event_list);
    if (timeout != NULL)
        apth_event_isolate(ev_timeout);

    // Select return code semantics
    if (ev_select->ev_status == APTH_EV_STATUS_FAILED)
    {
        apth_event_free(ev_select);
        if (ev_timeout != NULL)
            apth_event_free(ev_timeout);
        return apth_error(-1, EBADF);
    }

    // If the select event occurred, then RC should have been set in ev_args.SELECT.n
    // If timeout occurred and select event did not, return 0 and clear fd_set
    if (timeout != NULL &&
        ev_timeout->ev_status == APTH_EV_STATUS_OCCURRED &&
        ev_select->ev_status != APTH_EV_STATUS_OCCURRED)
    {
        if (rfds != NULL)
            FD_ZERO(rfds);
        if (wfds != NULL)
            FD_ZERO(wfds);
        if (efds != NULL)
            FD_ZERO(efds);
        rc = 0;
    }

    apth_event_free(ev_select);
    if (ev_timeout != NULL)
        apth_event_free(ev_timeout);

    return rc;
}

APTH_DEFINE_HOOK(
    int, pselect,
    (int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds, const struct timespec *ts, const sigset_t *mask),
    (nfds, rfds, wfds, efds, ts, mask))
{
    apth_event_t ev;
    apth_t cur = cur_apth();
    apth_debug("apth_func_pselect(hooked): called from thread \"%s\"", cur->name);

    // POSIX.1-2001/SUSv3 compliance
    if (nfds < 0 || nfds > FD_SETSIZE)
        return apth_error(-1, EINVAL);
    if (ts != NULL)
    {
        // Check timeout sanity
        if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000)
            return apth_error(-1, EINVAL);
        if (ts->tv_sec > 31 * 24 * 60 * 60) // 1 month
            return apth_error(-1, EINVAL);
    }

    // Handle signal mask if provided
    sigset_t origmask;
    if (mask != NULL)
    {
        if (sigprocmask(SIG_SETMASK, mask, &origmask) < 0)
            return apth_error(-1, errno);
    }

    // First deal with the special situation of a plain delay
    if (nfds == 0 && rfds == NULL && wfds == NULL && efds == NULL && ts != NULL)
    {
        long usec = ts->tv_sec * 1000000 + ts->tv_nsec / 1000;
        if (ts->tv_sec == 0 && usec < APTH_SELECT_DIRECT_TO_SCHED_THRESHOLD_US)
        {
            // Very small delays are acceptable to be performed directly
            struct timespec timeout_copy = *ts;
            while (apth_func_raw(pselect)(0, NULL, NULL, NULL, &timeout_copy, NULL) < 0 && errno == EINTR)
                ;
        }
        else
        {
            // Larger delays have to go through the scheduler
            ev = apth_event_time(APTH_EVENT_MODE_STATIC, apth_timeout(ts->tv_sec, ts->tv_nsec / 1000));
            apth_wait_event(ev);
            apth_event_free(ev);
        }

        if (mask != NULL)
            sigprocmask(SIG_SETMASK, &origmask, NULL);
        return 0;
    }

    // Now directly poll filedescriptor sets to avoid unnecessary event handling
    struct timespec delay;
    fd_set rspare, wspare, espare;
    fd_set *rtmp, *wtmp, *etmp;
    int rc;

    delay.tv_sec = 0;
    delay.tv_nsec = 0;
    rtmp = NULL;
    wtmp = NULL;
    etmp = NULL;
    if (rfds != NULL)
    {
        memcpy(&rspare, rfds, sizeof(fd_set));
        rtmp = &rspare;
    }
    if (wfds != NULL)
    {
        memcpy(&wspare, wfds, sizeof(fd_set));
        wtmp = &wspare;
    }
    if (efds != NULL)
    {
        memcpy(&espare, efds, sizeof(fd_set));
        etmp = &espare;
    }

    while ((rc = apth_func_raw(pselect)(nfds, rtmp, wtmp, etmp, &delay, NULL)) < 0 && errno == EINTR)
        ;
    if (rc < 0)
    {
        // Pass-through immediate error
        if (mask != NULL)
            sigprocmask(SIG_SETMASK, &origmask, NULL);
        return apth_error(-1, errno);
    }
    else if (rc > 0 || (rc == 0 && ts != NULL && ts->tv_sec == 0 && ts->tv_nsec == 0))
    {
        // Pass-through immediate success
        // Copy back results
        if (rfds != NULL)
            memcpy(rfds, &rspare, sizeof(fd_set));
        if (wfds != NULL)
            memcpy(wfds, &wspare, sizeof(fd_set));
        if (efds != NULL)
            memcpy(efds, &espare, sizeof(fd_set));
        if (mask != NULL)
            sigprocmask(SIG_SETMASK, &origmask, NULL);
        return rc;
    }

    // Suspend current apth until one filedescriptor is ready or the timeout occurred.
    apth_event_t ev_select;
    apth_event_t ev_timeout;
    struct list event_list;
    list_init(&event_list);
    rc = -1;
    ev = ev_select = apth_event_select(APTH_EVENT_MODE_STATIC, &rc, nfds, rfds, wfds, efds);
    apth_event_list_add(&event_list, ev);
    ev_timeout = NULL;
    if (ts != NULL)
    {
        ev_timeout = apth_event_time(APTH_EVENT_MODE_STATIC, apth_timeout(ts->tv_sec, ts->tv_nsec / 1000));
        apth_event_list_add(&event_list, ev_timeout);
    }
    apth_wait_event_list(&event_list);
    if (ts != NULL)
        apth_event_isolate(ev_timeout);

    // Select return code semantics
    if (ev_select->ev_status == APTH_EV_STATUS_FAILED)
    {
        apth_event_free(ev_select);
        if (ev_timeout != NULL)
            apth_event_free(ev_timeout);
        if (mask != NULL)
            sigprocmask(SIG_SETMASK, &origmask, NULL);
        return apth_error(-1, EBADF);
    }

    // If the select event occurred, then RC should have been set in ev_args.SELECT.n
    // If timeout occurred and select event did not, return 0 and clear fd_set
    if (ts != NULL &&
        ev_timeout->ev_status == APTH_EV_STATUS_OCCURRED &&
        ev_select->ev_status != APTH_EV_STATUS_OCCURRED)
    {
        if (rfds != NULL)
            FD_ZERO(rfds);
        if (wfds != NULL)
            FD_ZERO(wfds);
        if (efds != NULL)
            FD_ZERO(efds);
        rc = 0;
    }

    apth_event_free(ev_select);
    if (ev_timeout != NULL)
        apth_event_free(ev_timeout);

    // Restore original signal mask
    if (mask != NULL)
        sigprocmask(SIG_SETMASK, &origmask, NULL);

    return rc;
}

APTH_DEFINE_HOOK(int, poll,
                 (struct pollfd * fds, nfds_t nfds, int timeout),
                 (fds, nfds, timeout))
{
    if (nfds == 0)
    {
        if (timeout > 0)
        {
            usleep(timeout * 1000);
        }
        return 0;
    }

    // Do 0-timeout detection first
    int rc;
    while ((rc = apth_func_raw(poll)(fds, nfds, 0)) < 0 && errno == EINTR)
        ;
    if (rc > 0 || timeout == 0)
        return rc;

    // Construct event list, one fd event for every pollfd
    struct list event_list;
    list_init(&event_list);
    for (nfds_t i = 0; i < nfds; i++)
    {
        unsigned long goal = APTH_EVENT_MODE_STATIC;
        if (fds[i].events & POLLIN)
            goal |= APTH_GOAL_UNTIL_FD_READABLE;
        if (fds[i].events & POLLOUT)
            goal |= APTH_GOAL_UNTIL_FD_WRITEABLE;
        apth_event_t ev = apth_event_fd(goal, fds[i].fd);
        apth_event_list_add(&event_list, ev);
    }
    if (timeout > 0)
    {
        apth_event_t ev_timeout = apth_event_time(
            APTH_EVENT_MODE_STATIC,
            apth_timeout(timeout / 1000, (timeout % 1000) * 1000));
        apth_event_list_add(&event_list, ev_timeout);
    }
    apth_wait_event_list(&event_list);

    // poll again to fetch revents
    while ((rc = apth_func_raw(poll)(fds, nfds, 0)) < 0 && errno == EINTR)
        ;

    // TODO: a more general way for freeing event list
    while (!list_empty(&event_list))
    {
        struct list_elem *e = list_pop_front(&event_list);
        apth_event_t ev = apth_event_t_list_entry(e);
        apth_event_free(ev);
    }

    return rc;
}
