#ifndef __LIBAPTH_UTILS_LLL_NEW_H
#define __LIBAPTH_UTILS_LLL_NEW_H

#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>

// Forward declarations
typedef struct apth_st *apth_t;

/**
 * Type 1 LLL: Synchronization Primitives (APTH-only)
 *
 * Used for: mutex, cond, sem, rwlock, barrier guards
 * Characteristics:
 * - Stores APTH pointer to identify owner
 * - Only APTHs acquire these locks
 * - Yields on contention (lets scheduler run other APTHs)
 * - Provides trylock for non-blocking attempts
 */
typedef struct {
    _Atomic(apth_t) owner;  // NULL or APTH pointer
} lll_apth_t;

/**
 * Type 2 LLL: Internal LIBAPTH (Mixed, behavior depends on caller)
 *
 * Used for: queue locks, signal locks, pool locks, fd locks
 * Characteristics:
 * - Stores only a flag (0 or 1), no pointer needed
 * - Can be as small as unsigned char (1 byte)
 * - Behavior depends on caller:
 *   - APTH caller (LIBAPTH API): yields on contention
 *   - Scheduler caller (internal mechanism): spins on contention
 * - No trylock needed (internal code doesn't need non-blocking)
 */
typedef struct {
    _Atomic(unsigned char) locked;  // 0 or 1
} lll_internal_t;

// // Type 1 LLL API (APTH-only locks)
// void lll_apth_init(lll_apth_t *lock);
// void lll_apth_lock(lll_apth_t *lock);
// int  lll_apth_trylock(lll_apth_t *lock);  // Returns 0 on success, EBUSY on failure
// void lll_apth_unlock(lll_apth_t *lock);

// // Type 2 LLL API (Internal locks)
// void lll_internal_init(lll_internal_t *lock);
// void lll_internal_lock(lll_internal_t *lock);
// void lll_internal_unlock(lll_internal_t *lock);

// #define LLL_NOT_ACQUIRED (0) // LLL is not acquired

#define APTH_FAKE_SCHED_MARK_IN_PLACE (0x1)

// #define APTH_LLL_POINTER_MASK_IN_PLACE (~(APTH_FAKE_SCHED_MARK_IN_PLACE | APTH_LLL_ROBBED_MARK_IN_PLACE))

#define APTH_FAKE_SCHED(sched) ((apth_t)((uintptr_t)(sched) | APTH_FAKE_SCHED_MARK_IN_PLACE))
#define APTH_IS_FAKE_SCHED(val) (((uintptr_t)(val) & APTH_FAKE_SCHED_MARK_IN_PLACE) != 0)
// #define APTH_DECODE_FAKE_SCHED(val) ((void *)((uintptr_t)(val) & APTH_LLL_POINTER_MASK_IN_PLACE))
// #define APTH_LLL_IS_ROBBED(val) (((uintptr_t)(val) & APTH_LLL_ROBBED_MARK_IN_PLACE) != 0)

#endif // __LIBAPTH_UTILS_LLL_NEW_H
