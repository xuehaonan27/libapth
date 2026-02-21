#include "common.h"
#include "apth.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"

int apth_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    apth_t cur = cur_apth();

    if (oldset != NULL)
    {
        // Save current sigset_t
        *oldset = CTX_SIGMASK_OF(cur->ctx);
    }
    if (set == NULL)
    {
        return 0;
    }

    // Now `set` is not null
    sigset_t dest;
    switch (how)
    {
    case SIG_BLOCK:
        sigemptyset(&dest);
        for (int sig = 1; sig < APTH_NSIG; sig++)
            if (sigismember(&CTX_SIGMASK_OF(cur->ctx), sig) || sigismember(set, sig))
                sigaddset(&dest, sig);
        break;
    case SIG_UNBLOCK:
        dest = CTX_SIGMASK_OF(cur->ctx);
        for (int sig = 1; sig < APTH_NSIG; sig++)
            if (sigismember(set, sig))
                sigdelset(&dest, sig);
        break;
    case SIG_SETMASK:
        dest = *set;
        break;
    default:
        return apth_error(EINVAL, EINVAL);
    }
    CTX_SIGMASK_OF(cur->ctx) = dest;
    return 0;
}