/*
 * LIBAPTH epoll hooks for JVM integration.
 *
 * JDK NIO uses epoll_wait directly (via libnio's EPoll.c).  Without
 * hooking epoll_wait, an M:N thread calling epoll_wait blocks the
 * scheduler worker.  This hook converts epoll_wait into a cooperative wait.
 *
 * Strategy: call the real epoll_wait with timeout=0 first.  If no events are
 * ready, wait on the epoll fd itself through LIBAPTH's reactor.  Epoll fds are
 * pollable, so the reactor wakes this apth when the nested epoll instance has
 * events.  This avoids the previous epoll_wait(0)+yield loop that burned CPU
 * and starved JVM/Spark control paths.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/epoll.h>
#include <errno.h>
#include <time.h>

#include "hook_libc/hook_lowlevel_io.h"
#include "apth.h"
#include "apth_io.h"
#include "internal/apth_event.h"
#include "internal/types.h"
#include "utils/list.inline.h"

static long remaining_ms_until(const struct timespec *deadline)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long sec = deadline->tv_sec - now.tv_sec;
    long nsec = deadline->tv_nsec - now.tv_nsec;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }
    if (sec < 0)
        return 0;

    long ms = sec * 1000L + (nsec + 999999L) / 1000000L;
    return ms > 0 ? ms : 0;
}

APTH_API int apth_io_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
    {
        apth_t __ded_cur = CUR_APTH;
        if (__ded_cur == NULL || __ded_cur->is_dedicated) {
            if (apth_func_raw(epoll_wait) == NULL &&
                apth_func_init(epoll_wait)() != 0) {
                errno = ENOSYS;
                return -1;
            }
            return apth_func_raw(epoll_wait)(epfd, events, maxevents, timeout);
        }
    }
    apth_hook_debug(epoll_wait);

    if (apth_func_raw(epoll_wait) == NULL &&
        apth_func_init(epoll_wait)() != 0) {
        errno = ENOSYS;
        return -1;
    }

    /* Non-blocking poll (timeout == 0): passthrough. */
    if (timeout == 0)
        return apth_func_raw(epoll_wait)(epfd, events, maxevents, 0);

    /* M:N cooperative epoll_wait:
     * 1. Try non-blocking poll first.
     * 2. If no events, suspend this apth on the epoll fd readiness.
     * 3. When woken, retry raw epoll_wait(0) to consume exact events.
     */
    struct timespec deadline;
    if (timeout > 0)
    {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout / 1000;
        deadline.tv_nsec += (timeout % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L)
        {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    for (;;)
    {
        int n = apth_func_raw(epoll_wait)(epfd, events, maxevents, 0);
        if (n > 0)
            return n;
        if (n < 0 && errno != EINTR)
            return n; /* real error */

        if (n < 0 && errno == EINTR)
            continue;

        if (timeout > 0)
        {
            long rem_ms = remaining_ms_until(&deadline);
            if (rem_ms <= 0)
                return 0;

            struct apth_event_st ev_fd = EVENT_FD(epfd, APTH_GOAL_UNTIL_FD_READABLE);
            struct apth_event_st ev_timeout =
                EVENT_TIME(apth_timeout(rem_ms / 1000, (rem_ms % 1000) * 1000));
            struct list event_list;
            list_init(&event_list);
            apth_event_list_add(&event_list, &ev_fd);
            apth_event_list_add(&event_list, &ev_timeout);
            apth_wait_event_list(&event_list);

            if (ev_fd.ev_status == APTH_EV_STATUS_FAILED)
                return apth_error(-1, EBADF);
            if (ev_timeout.ev_status == APTH_EV_STATUS_OCCURRED &&
                ev_fd.ev_status != APTH_EV_STATUS_OCCURRED)
                return 0;
        }
        else
        {
            struct apth_event_st ev_fd = EVENT_FD(epfd, APTH_GOAL_UNTIL_FD_READABLE);
            apth_wait_event(&ev_fd);
            if (ev_fd.ev_status == APTH_EV_STATUS_FAILED)
                return apth_error(-1, EBADF);
        }
    }
}

APTH_DEFINE_HOOK(int, epoll_wait,
                 (int epfd, struct epoll_event *events, int maxevents, int timeout),
                 (epfd, events, maxevents, timeout))
{
    return apth_io_epoll_wait(epfd, events, maxevents, timeout);
}

APTH_DEFINE_HOOK(int, epoll_pwait,
                 (int epfd, struct epoll_event *events, int maxevents,
                  int timeout, const sigset_t *sigmask),
                 (epfd, events, maxevents, timeout, sigmask))
{
    {
        apth_t __ded_cur = CUR_APTH;
        if (__ded_cur == NULL || __ded_cur->is_dedicated)
            return apth_func_raw(epoll_pwait)(epfd, events, maxevents, timeout, sigmask);
    }
    apth_hook_debug(epoll_pwait);

    /* For M:N threads, ignore sigmask (software signal masks are per-apth,
     * not per-worker) and delegate to the cooperative epoll_wait. */
    return apth_func(epoll_wait)(epfd, events, maxevents, timeout);
}
