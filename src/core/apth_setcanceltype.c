#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"
#include "utils/atomic_wrapper.h"
#include "utils/debug.h"

int apth_setcanceltype(int type, int *oldtype)
{
    if (type < APTH_CANCEL_DEFERRED || type > APTH_CANCEL_ASYNCHRONOUS)
        return apth_error(EINVAL, EINVAL);

    volatile apth_t self = cur_apth();
    int oldval = atomic_load_relaxed(&self->cancelhandling);
    while (1)
    {
        int newval = (type == APTH_CANCEL_ASYNCHRONOUS
                          ? oldval | CANCELTYPE_BITMASK
                          : oldval & ~CANCELTYPE_BITMASK);

        if (oldtype != NULL)
            *oldtype = ((oldval & CANCELTYPE_BITMASK) ? APTH_CANCEL_ASYNCHRONOUS : APTH_CANCEL_DEFERRED);
        if (oldval == newval)
            break;

        if (atomic_compare_exchange_weak_acquire(&self->cancelhandling, &oldval, newval))
        {
            if (apth_cancel_enabled_and_canceled_and_async(newval))
            {
                self->join_arg = PTHREAD_CANCELED;
                apth_do_cancel(PTHREAD_CANCELED);
            }
            break;
        }
    }
    return 0;
}
