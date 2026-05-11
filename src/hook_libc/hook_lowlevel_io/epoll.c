/*
 * LIBAPTH epoll hooks for JVM integration.
 *
 * JDK NIO uses epoll_wait directly (via libnio's EPoll.c).  Without
 * hooking epoll_wait, an M:N thread calling epoll_wait blocks the
 * scheduler worker.  This hook converts epoll_wait into a cooperative
 * wait: if epoll_wait would block (returns 0 with timeout), the thread
 * yields to the scheduler and retries.
 *
 * Strategy: call the real epoll_wait with timeout=0 (non-blocking poll).
 * If events are ready, return them immediately.  If not, yield to the
 * scheduler and retry.  For infinite waits, the reactor will eventually
 * deliver events.
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

    /* M:N cooperative epoll_wait:
     * 1. Try non-blocking poll first.
     * 2. If no events, yield and retry until timeout expires or events arrive.
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

        /* Check timeout */
        if (timeout > 0)
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
                return 0; /* timed out */
        }

        /* Yield to let other M:N threads run, then retry. */
        apth_yield();
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
