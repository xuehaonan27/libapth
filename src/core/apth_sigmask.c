#include "common.h"
#include "apth.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"

int apth_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    apth_t cur = cur_apth();

    // If is scheduler context, operate with pthread level mask
    if (APTH_IS_FAKE_SCHED(cur))
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