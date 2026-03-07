#include "common.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"
#include "utils/lll_new.inline.h"  // NEW: Use new LLL types

// Raise a signal for an apth
int apth_kill(apth_t t, int sig)
{
    if (t == NULL || sig < 0 || sig >= APTH_NSIG)
        return apth_error(EINVAL, EINVAL);
    if (sig == 0)
        return apth_apth_exists(t) ? 0 : apth_error(ESRCH, ESRCH);

    // Check global action
    lll_internal_lock(&APTH_GLOBAL_SIGACTIONS.lock);  // NEW: Use Type 2 LLL
    struct sigaction sa = APTH_GLOBAL_SIGACTIONS.actions[sig];
    lll_internal_unlock(&APTH_GLOBAL_SIGACTIONS.lock);  // NEW: Use Type 2 LLL

    if (sa.sa_handler == SIG_IGN)
        return 0;

    // Atomically add `sig` to pending set of target apth `t`
    lll_internal_lock(&t->siglock);  // NEW: Use Type 2 LLL
    if (!sigismember(&t->sigpending, sig))
    {
        sigaddset(&t->sigpending, sig);
        t->sigpendcnt++;
    }
    lll_internal_unlock(&t->siglock);  // NEW: Use Type 2 LLL

    // Allow killing signal to self.
    // If `t == self`, check delivery immediately
    apth_t self = cur_apth();
    if (t == self && !APTH_IS_FAKE_SCHED(self))
        apth_deliver_pending_signals(self);

    // If `t` is in waiting queue and the signal is not blocked, we could
    // decide to wake it quickly, letting it handle the signal.
    // TODO: add a mark to `t`, when event manager detects this mark,
    // it could move `t` to waked queue.

    return 0;
}