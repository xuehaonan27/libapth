/*
 * LIBAPTH epoll hooks for JVM integration.
 *
 * JDK NIO uses epoll_wait directly (via libnio's EPoll.c).  Without
 * hooking epoll_wait, an M:N thread calling epoll_wait blocks the
 * scheduler worker.  This hook converts epoll_wait into a cooperative wait.
 *
 * Strategy: call the real epoll_wait with timeout=0 first.  Infinite waits
 * then wait on the epoll fd itself through LIBAPTH's reactor; epoll fds are
 * pollable, so the reactor wakes this apth when the nested epoll instance has
 * events.  Finite waits intentionally fall back to raw epoll_wait(timeout):
 * the current reactor cancellation path is asynchronous and unsafe for stack
 * allocated timeout events.
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

    /* Finite waits use the kernel.  This may block one worker pthread, but it
     * preserves correctness until reactor wait cancellation is made synchronous.
     */
    if (timeout > 0)
        return apth_func_raw(epoll_wait)(epfd, events, maxevents, timeout);

    /* M:N cooperative epoll_wait:
     * 1. Try non-blocking poll first.
     * 2. If no events, suspend this apth on the epoll fd readiness.
     * 3. When woken, retry raw epoll_wait(0) to consume exact events.
     */
    for (;;)
    {
        int n = apth_func_raw(epoll_wait)(epfd, events, maxevents, 0);
        if (n > 0)
            return n;
        if (n < 0 && errno != EINTR)
            return n; /* real error */

        if (n < 0 && errno == EINTR)
            continue;

        struct apth_event_st ev_fd = EVENT_FD(epfd, APTH_GOAL_UNTIL_FD_READABLE);
        apth_wait_event(&ev_fd);
        if (ev_fd.ev_status == APTH_EV_STATUS_FAILED)
            return apth_error(-1, EBADF);
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
