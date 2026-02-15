#include "lll.h"
#include "atomic_wrapper.h"
#include "archplattoold.h"

void lll_lock(lll_t *lock)
{
    _Atomic int *inner = &lock->inner;
    if (apth_unlikely(atomic_compare_and_exchange_bool_acq(
            inner, LLL_ACQUIRED_NO_WAITERS, LLL_NOT_ACQUIRED)))
    {
        lll_lock_wait(inner);
    }
}

void lll_lock_wait(_Atomic int *inner)
{
    if (atomic_load_relaxed(inner) == LLL_ACQUIRED_WITH_WAITERS)
    {
    }

    while (atomic_exchange_acquire(inner, LLL_ACQUIRED_WITH_WAITERS) != 0)
    {
        // TODO:
    }
}
