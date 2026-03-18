#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <signal.h>
#include <malloc.h>

#include "hook_libc/hook_lowlevel_io.h"
#include "apth.h"
#include "internal/apth_event.h"
#include "internal/types.h"
#include "utils/list.inline.h"

// Maximum number of decomposed FD events before falling back to SELECT event.
// Keeps stack usage bounded (128 * sizeof(struct apth_event_st) ≈ 12KB).
#define SELECT_DECOMPOSE_MAX 128

// Count set fds across all three fd_sets, up to nfd.
static int select_count_fds(int nfd, fd_set *rfds, fd_set *wfds, fd_set *efds)
{
    int count = 0;
#if defined(__linux__) && defined(__NFDBITS)
    int nwords = ((nfd) + __NFDBITS - 1) / __NFDBITS;
    for (int i = 0; i < nwords; i++)
    {
        if (rfds != NULL)
            count += __builtin_popcountl(rfds->fds_bits[i]);
        if (wfds != NULL)
            count += __builtin_popcountl(wfds->fds_bits[i]);
        if (efds != NULL)
            count += __builtin_popcountl(efds->fds_bits[i]);
    }
#else
    for (int s = 0; s < nfd; s++)
    {
        if (rfds != NULL && FD_ISSET(s, rfds))
            count++;
        if (wfds != NULL && FD_ISSET(s, wfds))
            count++;
        if (efds != NULL && FD_ISSET(s, efds))
            count++;
    }
#endif
    return count;
}

// Decompose fd_sets into individual FD events. Returns number of events created.
// A single fd in multiple sets generates multiple events (each with its own goal).
static int select_decompose_events(int nfd, fd_set *rfds, fd_set *wfds, fd_set *efds,
                                   struct apth_event_st *evs, int max_evs)
{
    int idx = 0;
    for (int s = 0; s < nfd && idx < max_evs; s++)
    {
        if (rfds != NULL && FD_ISSET(s, rfds))
        {
            struct apth_event_st ev = EVENT_FD(s, APTH_GOAL_UNTIL_FD_READABLE);
            evs[idx++] = ev;
        }
        if (wfds != NULL && FD_ISSET(s, wfds) && idx < max_evs)
        {
            struct apth_event_st ev = EVENT_FD(s, APTH_GOAL_UNTIL_FD_WRITEABLE);
            evs[idx++] = ev;
        }
        if (efds != NULL && FD_ISSET(s, efds) && idx < max_evs)
        {
            struct apth_event_st ev = EVENT_FD(s, APTH_GOAL_UNTIL_FD_EXCEPTION);
            evs[idx++] = ev;
        }
    }
    return idx;
}

APTH_DEFINE_HOOK(
    int, select,
    (int nfd, fd_set *rfds, fd_set *wfds, fd_set *efds, struct timeval *timeout),
    (nfd, rfds, wfds, efds, timeout))
{
    apth_t cur = CUR_APTH;
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
            struct apth_event_st ev = EVENT_TIME(apth_timeout(timeout->tv_sec, timeout->tv_usec));
            apth_wait_event(&ev);
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

    // Suspend current apth until one filedescriptor is ready or the timeout occurred.
    // Decompose fd_sets into individual FD events for epoll-based waiting,
    // eliminating the per-iteration raw select() syscall in the event manager.
    int fd_count = select_count_fds(nfd, rfds, wfds, efds);

    struct list event_list;
    list_init(&event_list);
    rc = -1;

    if (fd_count > 0 && fd_count <= SELECT_DECOMPOSE_MAX)
    {
        // Decompose into individual FD events
        struct apth_event_st evs[fd_count];
        int nev = select_decompose_events(nfd, rfds, wfds, efds, evs, fd_count);
        for (int i = 0; i < nev; i++)
            apth_event_list_add(&event_list, &evs[i]);

        struct apth_event_st ev_timeout = EVENT_TIME(apth_timeout(timeout->tv_sec, timeout->tv_usec));
        if (timeout != NULL)
            apth_event_list_add(&event_list, &ev_timeout);

        apth_wait_event_list(&event_list);

        // Check if any FD event occurred or all failed
        bool any_occurred = false;
        bool any_failed = false;
        for (int i = 0; i < nev; i++)
        {
            if (evs[i].ev_status == APTH_EV_STATUS_OCCURRED)
                any_occurred = true;
            else if (evs[i].ev_status == APTH_EV_STATUS_FAILED)
                any_failed = true;
        }

        if (any_failed && !any_occurred)
            return apth_error(-1, EBADF);

        if (any_occurred)
        {
            // Do a final raw select to get precise output fd_sets
            struct timeval zero_tv = {0, 0};
            while ((rc = apth_func_raw(select)(nfd, rfds, wfds, efds, &zero_tv)) < 0 && errno == EINTR)
                ;
            if (rc < 0)
                return apth_error(-1, errno);
        }
        else
        {
            // Timeout: no fds ready
            if (rfds != NULL)
                FD_ZERO(rfds);
            if (wfds != NULL)
                FD_ZERO(wfds);
            if (efds != NULL)
                FD_ZERO(efds);
            rc = 0;
        }
    }
    else
    {
        // Fallback: use SELECT event for large fd counts
        struct apth_event_st ev_select = EVENT_SELECT(&rc, nfd, rfds, wfds, efds);
        apth_event_list_add(&event_list, &ev_select);

        struct apth_event_st ev_timeout = EVENT_TIME(apth_timeout(timeout->tv_sec, timeout->tv_usec));
        if (timeout != NULL)
            apth_event_list_add(&event_list, &ev_timeout);

        apth_wait_event_list(&event_list);

        if (ev_select.ev_status == APTH_EV_STATUS_FAILED)
            return apth_error(-1, EBADF);

        if (timeout != NULL &&
            ev_timeout.ev_status == APTH_EV_STATUS_OCCURRED &&
            ev_select.ev_status != APTH_EV_STATUS_OCCURRED)
        {
            if (rfds != NULL)
                FD_ZERO(rfds);
            if (wfds != NULL)
                FD_ZERO(wfds);
            if (efds != NULL)
                FD_ZERO(efds);
            rc = 0;
        }
    }

    return rc;
}

APTH_DEFINE_HOOK(
    int, pselect,
    (int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds, const struct timespec *ts, const sigset_t *mask),
    (nfds, rfds, wfds, efds, ts, mask))
{
    apth_t cur = CUR_APTH;
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
            struct apth_event_st ev = EVENT_TIME(apth_timeout(ts->tv_sec, ts->tv_nsec / 1000));
            apth_wait_event(&ev);
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
    int fd_count = select_count_fds(nfds, rfds, wfds, efds);

    struct list event_list;
    list_init(&event_list);
    rc = -1;

    if (fd_count > 0 && fd_count <= SELECT_DECOMPOSE_MAX)
    {
        // Decompose into individual FD events
        struct apth_event_st evs[fd_count];
        int nev = select_decompose_events(nfds, rfds, wfds, efds, evs, fd_count);
        for (int i = 0; i < nev; i++)
            apth_event_list_add(&event_list, &evs[i]);

        struct apth_event_st ev_timeout = EVENT_TIME(apth_timeout(ts->tv_sec, ts->tv_nsec / 1000));
        if (ts != NULL)
            apth_event_list_add(&event_list, &ev_timeout);

        apth_wait_event_list(&event_list);

        bool any_occurred = false;
        bool any_failed = false;
        for (int i = 0; i < nev; i++)
        {
            if (evs[i].ev_status == APTH_EV_STATUS_OCCURRED)
                any_occurred = true;
            else if (evs[i].ev_status == APTH_EV_STATUS_FAILED)
                any_failed = true;
        }

        if (any_failed && !any_occurred)
        {
            if (mask != NULL)
                sigprocmask(SIG_SETMASK, &origmask, NULL);
            return apth_error(-1, EBADF);
        }

        if (any_occurred)
        {
            struct timeval zero_tv = {0, 0};
            while ((rc = apth_func_raw(select)(nfds, rfds, wfds, efds, &zero_tv)) < 0 && errno == EINTR)
                ;
            if (rc < 0)
            {
                if (mask != NULL)
                    sigprocmask(SIG_SETMASK, &origmask, NULL);
                return apth_error(-1, errno);
            }
        }
        else
        {
            if (rfds != NULL)
                FD_ZERO(rfds);
            if (wfds != NULL)
                FD_ZERO(wfds);
            if (efds != NULL)
                FD_ZERO(efds);
            rc = 0;
        }
    }
    else
    {
        // Fallback: use SELECT event for large fd counts
        struct apth_event_st ev_select = EVENT_SELECT(&rc, nfds, rfds, wfds, efds);
        apth_event_list_add(&event_list, &ev_select);

        struct apth_event_st ev_timeout = EVENT_TIME(apth_timeout(ts->tv_sec, ts->tv_nsec / 1000));
        if (ts != NULL)
            apth_event_list_add(&event_list, &ev_timeout);

        apth_wait_event_list(&event_list);

        if (ev_select.ev_status == APTH_EV_STATUS_FAILED)
        {
            if (mask != NULL)
                sigprocmask(SIG_SETMASK, &origmask, NULL);
            return apth_error(-1, EBADF);
        }

        if (ts != NULL &&
            ev_timeout.ev_status == APTH_EV_STATUS_OCCURRED &&
            ev_select.ev_status != APTH_EV_STATUS_OCCURRED)
        {
            if (rfds != NULL)
                FD_ZERO(rfds);
            if (wfds != NULL)
                FD_ZERO(wfds);
            if (efds != NULL)
                FD_ZERO(efds);
            rc = 0;
        }
    }

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

    // Prepare an array of events. Thankfully, we are in C, not CPP.
    struct apth_event_st evs[nfds];

    // Construct event list, one fd event for every pollfd
    struct list event_list;
    list_init(&event_list);
    for (nfds_t i = 0; i < nfds; i++)
    {
        unsigned long goal = 0;
        if (fds[i].events & POLLIN)
            goal |= APTH_GOAL_UNTIL_FD_READABLE;
        if (fds[i].events & POLLOUT)
            goal |= APTH_GOAL_UNTIL_FD_WRITEABLE;
        struct apth_event_st ev = EVENT_FD(fds[i].fd, goal);
        evs[i] = ev;
        apth_event_list_add(&event_list, &evs[i]);
    }
    struct apth_event_st ev_timeout = EVENT_TIME(apth_timeout(timeout / 1000, (timeout % 1000) * 1000));
    if (timeout > 0)
        apth_event_list_add(&event_list, &ev_timeout);
    apth_wait_event_list(&event_list);

    // poll again to fetch revents
    while ((rc = apth_func_raw(poll)(fds, nfds, 0)) < 0 && errno == EINTR)
        ;

    return rc;
}
