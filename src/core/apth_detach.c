#include "apth.h"
#include "internal/types.h"
#include "hook_libc/hooked_funcs.h"
#include "utils/atomic_wrapper.h"
#include "utils/apth_errno.h"
#include "utils/debug.h"

int apth_detach(apth_t th)
{
    // TODO: make sure the th is valid.
    if (!APTH_IS_VALID(th))
        return apth_error(ESRCH, ESRCH);

    // Mark the thread as detached
    if (atomic_compare_and_exchange_bool_acq(&th->joinid, th, NULL))
    {
        // Two possibilities here.
        // First, the thread might already be detached, then return EINVAL.
        // Otherwise there might already be a waiter.
        if (IS_DETACHED(th))
            return apth_error(EINVAL, EINVAL);
        else
        {
            // Another thread is already joining; cannot detach.
            return apth_error(EINVAL, EINVAL);
        }
    }

    // For dedicated threads, detach the backing pthread
    if (th->is_dedicated)
        pthread_detach(th->dedicated_tid);

    return 0;
}