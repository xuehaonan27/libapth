#ifndef __LIBAPTH_UTILS_LLL_H
#define __LIBAPTH_UTILS_LLL_H

typedef struct lowlevellock
{
    _Atomic int inner;
} lll_t;

#define LLL_NOT_ACQUIRED (0)          // LLL is not acquired
#define LLL_ACQUIRED_NO_WAITERS (1)   // LLL is acquired but without waiters
#define LLL_ACQUIRED_WITH_WAITERS (2) // LLL is acquired with waiters

void lll_lock(lll_t *lock);
void lll_lock_wait(_Atomic int *inner);

#endif // __LIBAPTH_UTILS_LLL_H
