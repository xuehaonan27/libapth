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
    if (th == APTH_NULL || !APTH_IS_VALID(th))
        return apth_error(ESRCH, ESRCH);
    // apth_sched_t sched = cur_sched();

    // The current thread cannot be cancelled
    // if (th == sched->cur)
    //     return apth_error(EINVAL, EINVAL);

    // if (th->state == APTH_STATE_TERMINATED)
    if (state_holder_of(th) == APTH_STATE_TERMINATED)
        // return apth_error(EPERM, EPERM);
        return 0;

    // Now mark the thread as cancelled
    // TODO: atomicity
    th->cancelreq = true;

    // TODO: When the cancellation type of `th` is async!
    // TODO: Then there should be some way to notify the scheduler of `th`
    // of the information that this `th` should be scheduled as soon as
    // possible.
    // TODO: maybe set a high priority to `th` is good?

    return 0;
}
