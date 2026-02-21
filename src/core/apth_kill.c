#include "common.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"

// Raise a signal for an apth
int apth_kill(apth_t t, int sig)
{
    struct sigaction sa;

    apth_t cur = cur_apth();

    // TODO: should t == cur be an error case?
    if (t == NULL || t == cur || (sig < 0 || sig > APTH_NSIG))
        return apth_error(EINVAL, EINVAL);

    if (sig == 0)
        // Just perform a check
        return apth_apth_exists(t);

    // Raise signal for t
    if (sigaction(sig, NULL, &sa) != 0)
        return apth_error(EINVAL, EINVAL);
    // Check the global handler of `sig`
    if (sa.sa_handler == SIG_IGN)
        return 0; // Fine, nothing to do, sig is globally ignored

    // TODO: atomicity should ensured, lock the signal system of `t`
    if (!sigismember(&t->sigpending, sig))
    {
        sigaddset(&t->sigpending, sig);
        t->sigpendcnt++;
    }

    // NOTE: containing a cancelation point
    apth_yield();
    return 0;
}