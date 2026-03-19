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

    // If is scheduler context, operate with pthread level mask
    if (cur == NULL)
        return apth_func_raw(pthread_sigmask)(how, set, oldset);

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