#ifndef __LIBAPTH_UTILS_LLL_H
#define __LIBAPTH_UTILS_LLL_H

#include <stdint.h>
#include <stdatomic.h>
#include "utils/archplattoold.h"

/**
 * Low-level locks use a combination of atomic operations (to acquire and
 * release lock ownership) and cooperation of APTH scheduling system.
 * A lock can be in one of the three states (`ptcb` refers to `poiner to TCB`,
 * and `psched` refers to `pointer to scheduler`), stored in `inner` field:
 * 
 *  0x0           : not acquired.
 *  (ptcb)        : this lock is held by an apth, and the `ptcb` points to the TCB.
 *  (psched) | 0x1: acquired by a scheduler, not an apth.
 *  (ptcb) | 0x2  : robbed lock. This lock is originally held by an apth, and 
 *                  this apth is parked while still holding the lock, but its scheduler 
 *                  need this very lock then. So the scheduler could "rob" the lock from 
 *                  its apth workload temporarily. The robber scheduler could be got via
 *                  `(ptcb)->sched`.
 * s
 * Since TCB is assured to be at least 4 bytes aligned, we can directly use the 
 * lower bits of the pointer value.
 */

typedef struct lowlevellock
{
    _Atomic uintptr_t inner;
} lll_t;

#define LLL_NOT_ACQUIRED (0) // LLL is not acquired

#define APTH_FAKE_SCHED_MARK_IN_PLACE (0x1)

// The lll is robbed, meaning the original owner is an apth, but its scheduler
// robbed the lock for itself. The pointer is still the apth, and the robber
// is easy to get via `owner->worker`.
#define APTH_LLL_ROBBED_MARK_IN_PLACE (0x2)
#define APTH_LLL_POINTER_MASK_IN_PLACE (~(APTH_FAKE_SCHED_MARK_IN_PLACE | APTH_LLL_ROBBED_MARK_IN_PLACE))

#define APTH_FAKE_SCHED(sched) ((apth_t)((uintptr_t)(sched) | APTH_FAKE_SCHED_MARK_IN_PLACE))
#define APTH_IS_FAKE_SCHED(val) (((uintptr_t)(val) & APTH_FAKE_SCHED_MARK_IN_PLACE) != 0)
#define APTH_DECODE_FAKE_SCHED(val) ((void *)((uintptr_t)(val) & APTH_LLL_POINTER_MASK_IN_PLACE))
#define APTH_LLL_IS_ROBBED(val) (((uintptr_t)(val) & APTH_LLL_ROBBED_MARK_IN_PLACE) != 0)

APTH_INTERNAL void lll_init(lll_t *lock);
// #if defined(APTH_DEBUG) && defined(APTH_DEBUG_LLL)
APTH_INTERNAL void lll_lock(lll_t *lock, const char *from);
APTH_INTERNAL void lll_unlock(lll_t *lock, const char *from);
// #else  // APTH_DEBUG_LLL
// APTH_INTERNAL void lll_lock(lll_t *lock);
// APTH_INTERNAL void lll_unlock(lll_t *lock);
// #endif // APTH_DEBUG_LLL

#endif // __LIBAPTH_UTILS_LLL_H
