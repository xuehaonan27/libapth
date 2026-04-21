#ifndef _GNU_SOURCE
#define _GNU_SOURCE // For SIG_SETMASK
#endif
#include <signal.h>

#include "apth.h"
#include "internal/types.h"
#include "internal/apth_signal.h"
#include "hook_libc/hooked_funcs.h"
#include "utils/apth_errno.h"

int apth_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    apth_t cur = CUR_APTH;

    // If scheduler context, post-shutdown, or DEDICATED thread, operate
    // at kernel level.  CUR_APTH is NULL in scheduler context, during
    // shutdown after the apth has exited, and when a signal fires on a
    // worker that has no dispatched thread.  DEDICATED threads need real
    // pthread_sigmask because they are 1:1 kernel threads and the kernel
    // signal mask must be authoritative.
    if (cur == NULL || cur->is_dedicated)
    {
        // apth_func_raw(pthread_sigmask) is NULL before apth_hook_init().
        // Fall back to the direct libc call when the hook table is empty.
        int (*raw_fn)(int, const sigset_t *, sigset_t *) =
            apth_func_raw(pthread_sigmask);
        if (raw_fn == NULL)
            raw_fn = pthread_sigmask;
        int ret = raw_fn(how, set, oldset);
        // Keep TCB sigmask in sync for DEDICATED threads
        if (ret == 0 && cur != NULL && cur->is_dedicated)
        {
            raw_fn(SIG_SETMASK, NULL, &cur->sigmask);
        }
        return ret;
    }

    if (oldset != NULL)
        *oldset = cur->sigmask;

    if (set == NULL)
        return 0;

    switch (how)
    {
    case SIG_BLOCK:
        for (int sig = 1; sig < APTH_NSIG; sig++)
            if (sigismember(set, sig))
                sigaddset(&cur->sigmask, sig);
        break;
    case SIG_UNBLOCK:
        for (int sig = 1; sig < APTH_NSIG; sig++)
            if (sigismember(set, sig))
                sigdelset(&cur->sigmask, sig);
        break;
    case SIG_SETMASK:
        cur->sigmask = *set;
        break;
    default:
        return EINVAL;
    }

    // SIGKILL and SIGSTOP cannot be blocked
    sigdelset(&cur->sigmask, SIGKILL);
    sigdelset(&cur->sigmask, SIGSTOP);

    // Check if there could be new arriving signals after modified the mask.
    // The signal might already pending there.
    if (how == SIG_UNBLOCK || how == SIG_SETMASK)
    {
        if (cur->sigpendcnt > 0)
            apth_deliver_pending_signals(cur);
    }

    return 0;
}