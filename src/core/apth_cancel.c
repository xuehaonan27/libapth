#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"

// Cancel an apth. It sends a cancelation request to the apth. Whether and
// when the target apth reacts to the cancelation request depends on two
// attirbutes that are under the control of that thread: its cancelability
// state and type.
int apth_cancel(apth_t th)
{
    if (th == APTH_NULL)
        return apth_error(EINVAL, EINVAL);
    // The current thread cannot be calcelled
    apth_sched_t sched = cur_sched();
    if (th == sched->cur)
        return apth_error(EINVAL, EINVAL);

    // The thread has to be at least still alive
    if (th->state == APTH_STATE_TERMINATED)
        return apth_error(EPERM, EPERM);

    // Now mark the thread as cancelled
    // TODO: atomicity
    th->cancelreq = true;

    unsigned int cc_h = atomic_load_acquire(&th->cancelhandling);
    // When cancellation is enabled in async mode we cancel the thread immediately
    if ((cc_h & CANCELSTATE_BITMASK) == 0 /* Cancel enabled */
        && (cc_h & CANCELTYPE_BITMASK) != 0 /* Asynchronous */)
    {
        // Remove thread from its queue
        // TODO: lock the thread list
        // list_remove(&th->elem);
        remove_apth(th);

        // Execute cleanups
        apth_thread_clenaup(th);

        // And now either kick it out or move it to dead queue
        if (IS_DETACHED(th))
        {
            // `th` is detached, directly free
            apth_debug("apth_cancel: kicking out cancelled thread \"%s\" immediately", th->name);
            apth_tcb_free(th);
        }
        else
        {
            // Someone is waiting for `th`
            // TODO: here we must yield to scheduler
            apth_debug("apth_cancel: moving cancelled thread \"%s\" to dead queue", th->name);
            th->join_arg = APTH_CANCELED;
            th->state = APTH_STATE_TERMINATED;
            // push_apth_to_terminated(th, sched);
            apth_yield();
        }
    }

    return 0;
}
