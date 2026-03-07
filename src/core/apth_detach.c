#include "apth.h"
#include "internal/apth_tcb.h"
#include "utils/atomic_wrapper.h"
#include "utils/apth_errno.h"
#include "utils/debug.h"

int apth_detach(apth_t th)
{
    // TODO: make sure the th is valid.
    if (!APTH_IS_VALID(th))
        return apth_error(ESRCH, ESRCH);

    // Mark te thread as detached
    if (atomic_compare_and_exchange_bool_acq(&th->joinid, th, NULL))
    {
        // Two possibilities here.
        // First, the thread might already be detached, then return EINVAL.
        // Otherwise there might already be a waiter. No know what to do in
        // this situation.
        if (IS_DETACHED(th))
            // result = EINVAL;
            return apth_error(EINVAL, EINVAL);
        else
        {
            // TODO: Check whether the thread terminated meanwhile.
            TODO("Check");
        }
    }

    return 0;
}