#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <signal.h>

#include "apth.h"
#include "internal/types.h"
#include "internal/apth_thqueue.h"
#include "internal/apth_sched.h"
#include "internal/apth_signal.h"
#include "hook_libc/hooked_funcs.h"
#include "utils/apth_errno.h"
#include "utils/lll.inline.h"

static int apth_apth_exists(apth_t t)
{
    if (!APTH_IS_VALID(t))
        return false;

    apth_sched_t sched = CUR_SCHED;

    bool found_in_new = apth_is_in(THQUEUE(sched, new), t);
    bool found_in_ready = apth_is_in(THQUEUE(sched, ready), t);
    bool found_in_waiting = apth_is_in(THQUEUE(sched, waiting), t);
    bool found_in_terminated = apth_is_in(THQUEUE(sched, terminated), t);
    bool found_in_waked = apth_is_in(THQUEUE(sched, waked), t);

    return found_in_new || found_in_ready || found_in_waiting || found_in_terminated || found_in_waked;
}

// Raise a signal for an apth
int apth_kill(apth_t t, int sig)
{
    if (t == NULL || sig < 0 || sig >= APTH_NSIG)
        return apth_error(EINVAL, EINVAL);
    if (sig == 0)
        return apth_apth_exists(t) ? 0 : apth_error(ESRCH, ESRCH);

    // Check global action
    lll_internal_lock(&APTH_GLOBAL_SIGACTIONS.lock);
    struct sigaction sa = APTH_GLOBAL_SIGACTIONS.actions[sig];
    lll_internal_unlock(&APTH_GLOBAL_SIGACTIONS.lock);

    if (sa.sa_handler == SIG_IGN)
        return 0;

    // Atomically add `sig` to pending set of target apth `t`
    lll_internal_lock(&t->siglock);
    if (!sigismember(&t->sigpending, sig))
    {
        sigaddset(&t->sigpending, sig);
        t->sigpendcnt++;
    }
    lll_internal_unlock(&t->siglock);

    // Self-signal: immediate delivery
    apth_t self = CUR_APTH;
    if (t == self && self != NULL)
    {
        apth_deliver_pending_signals(self);
        return 0;
    }

    // DEDICATED thread: forward to pthread_kill for immediate kernel delivery
    if (t->is_dedicated)
    {
        return apth_func_raw(pthread_kill)(t->dedicated_tid, sig);
    }

    // M:N thread: wake if not running so scheduler can deliver signal.
    // The scheduler delivers pending signals before entering user code.
    int state = atomic_load_acquire(&t->state);
    if (state == APTH_STATE_WAITING ||
        state == APTH_STATE_READY ||
        state == APTH_STATE_WAKED)
    {
        apth_sched_t sched = t->home_sched;
        if (sched != NULL)
        {
            apth_sched_wake_thread(sched, t);
        }
    }
    // If RUNNING: signal is pending, delivered on next yield/preemption check.

    return 0;
}