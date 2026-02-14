#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/apth_errno.h"

int apth_setcancelstate(int state, int *oldstate)
{
    if (!(state == APTH_CANCEL_ENABLE || state == APTH_CANCEL_DISABLE))
        return apth_error(EINVAL, EINVAL);

    volatile apth_t self = cur_apth();

    int oldval = atomic_load_relaxed(&self->cancelhandling);
    while (1)
    {
        int newval = (state == APTH_CANCEL_DISABLE
                          ? oldval | CANCELSTATE_BITMASK
                          : oldval & ~CANCELSTATE_BITMASK);
        if (oldstate != NULL)
            *oldstate = ((oldval & CANCELSTATE_BITMASK) ? APTH_CANCEL_DISABLE : APTH_CANCEL_ENABLE);
        if (oldval == newval)
            break;

        if (atomic_compare_exchange_weak_acquire(&self->cancelhandling, &oldval, newval))
        {
            if (apth_cancel_enabled_and_canceled_and_async(newval))
                apth_do_cancel(APTH_CANCELED);
            break;
        }
    }

    return 0;
}
