#ifndef __LIBAPTH_UTILS_LLL_H
#define __LIBAPTH_UTILS_LLL_H

#include "atomic_wrapper.h"

typedef struct lowlevellock
{
    int inner;
} lll_t;

#define LLL_NOT_ACQUIRED (0)          // LLL is not acquired
#define LLL_ACQUIRED_NO_WAITERS (1)   // LLL is acquired but without waiters
#define LLL_ACQUIRED_WITH_WAITERS (2) // LLL is acquired with waiters

void lll_lock(lll_t *lock)
{
    int *inner = &lock->inner;
    if (apth_unlikely(atomic_compare_and_exchange_bool_acq(
            inner, LLL_ACQUIRED_NO_WAITERS, LLL_NOT_ACQUIRED)))
    {
        lll_lock_wait(inner);
    }
}

void lll_lock_wait(int *inner)
{
    if (atomic_load_relaxed(inner) == LLL_ACQUIRED_WITH_WAITERS)
    {
    }

    while (atomic_exchange_acquire(inner, LLL_ACQUIRED_WITH_WAITERS) != 0)
    {
        // TODO:
    }
}

#endif // __LIBAPTH_UTILS_LLL_H
