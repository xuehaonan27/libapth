#ifndef __LIBAPTH_UTILS_LLL_NEW_INLINE_H
#define __LIBAPTH_UTILS_LLL_NEW_INLINE_H

#include "common.h"
#include "apth.h"
#include "lll.h"
#include "hook_libc/hooked_funcs.h"
#include "internal/forward_declare.h"
#include "internal/types.h"
#include "utils/archplattoold.h"
#include "utils/atomic_wrapper.h"
#include "utils/debug.h"

// ==================== Type 1 LLL: APTH-only locks ====================

INLINE_ALWAYS void lll_apth_init(lll_apth_t *lock)
{
    atomic_init(&lock->owner, NULL);
}

#if defined(APTH_DEBUG) && defined(APTH_DEBUG_LLL)
INLINE_ALWAYS void __lll_apth_lock(lll_apth_t *lock, const char *from)
{
    apth_debug("lll_apth_lock from=%s", from);
#else
INLINE_ALWAYS void __lll_apth_lock(lll_apth_t *lock)
{
#endif
    apth_t self = CUR_APTH;

    // Sanity check: only APTHs can acquire Type 1 locks
    assert_msg(self != NULL, "Type 1 LLL can only be acquired by APTHs, not scheduler context");

    // Fast path: try to acquire
    apth_t expected = NULL;
    if (atomic_compare_exchange_acquire(&lock->owner, &expected, self))
        return;

    // Slow path: yield on contention
    while (1)
    {
        apth_t owner = atomic_load_acquire(&lock->owner);

        if (owner == NULL)
        {
            expected = NULL;
            if (atomic_compare_exchange_acquire(&lock->owner, &expected, self))
                return;
            continue;
        }

        // Yield to scheduler, let it run other APTHs
        apth_yield();
    }
}

#if defined(APTH_DEBUG) && defined(APTH_DEBUG_LLL)
INLINE_ALWAYS int __lll_apth_trylock(lll_apth_t *lock, const char *from)
{
    apth_debug("lll_apth_trylock from=%s", from);
#else
INLINE_ALWAYS int __lll_apth_trylock(lll_apth_t *lock)
{
#endif
    apth_t self = CUR_APTH;

    // Sanity check: only APTHs can acquire Type 1 locks
    assert_msg(self != NULL, "Type 1 LLL can only be acquired by APTHs, not scheduler context");

    apth_t expected = NULL;
    if (atomic_compare_exchange_acquire(&lock->owner, &expected, self))
        return 0;  // Success

    return EBUSY;  // Lock is held
}

#if defined(APTH_DEBUG) && defined(APTH_DEBUG_LLL)
INLINE_ALWAYS void __lll_apth_unlock(lll_apth_t *lock, const char *from)
{
    apth_debug("lll_apth_unlock from=%s", from);
#else
INLINE_ALWAYS void __lll_apth_unlock(lll_apth_t *lock)
{
#endif
    apth_t self = CUR_APTH;
    apth_t expected = self;

    if (!atomic_compare_exchange_release(&lock->owner, &expected, NULL))
    {
        PANIC("lll_apth_unlock: unlock by non-owner");
        apth_func_raw(exit)(127);
    }
}

// ==================== Type 2 LLL: Internal locks ====================

INLINE_ALWAYS void lll_internal_init(lll_internal_t *lock)
{
    atomic_init(&lock->locked, 0);
}

#if defined(APTH_DEBUG) && defined(APTH_DEBUG_LLL)
INLINE_ALWAYS void __lll_internal_lock(lll_internal_t *lock, const char *from)
{
    apth_debug("lll_internal_lock from=%s", from);
#else
INLINE_ALWAYS void __lll_internal_lock(lll_internal_t *lock)
{
#endif
    apth_sched_t cur_sched = CUR_SCHED;
    apth_t self = (cur_sched != NULL) ? cur_sched->cur : NULL;
    bool is_scheduler = (self == NULL);

    // Fast path: try to acquire
    unsigned char expected = 0;
    if (atomic_compare_exchange_acquire(&lock->locked, &expected, 1))
        return;

    // Slow path: behavior depends on caller
    while (1)
    {
        unsigned char val = atomic_load_acquire(&lock->locked);

        if (val == 0)
        {
            expected = 0;
            if (atomic_compare_exchange_acquire(&lock->locked, &expected, 1))
                return;
            continue;
        }

        if (is_scheduler)
        {
            // Scheduler: spin (critical sections are short)
            // Use sched_yield to avoid busy-waiting
            sched_yield();
        }
        else
        {
            // APTH: yield to scheduler
            apth_yield();
        }
    }
}

#if defined(APTH_DEBUG) && defined(APTH_DEBUG_LLL)
INLINE_ALWAYS void __lll_internal_unlock(lll_internal_t *lock, const char *from)
{
    apth_debug("lll_internal_unlock from=%s", from);
#else
INLINE_ALWAYS void __lll_internal_unlock(lll_internal_t *lock)
{
#endif
    unsigned char expected = 1;

    if (!atomic_compare_exchange_release(&lock->locked, &expected, 0))
    {
        PANIC("lll_internal_unlock: unlock of non-acquired lock");
        apth_func_raw(exit)(127);
    }
}

// Debug macros
#if defined(APTH_DEBUG) && defined(APTH_DEBUG_LLL)
#define lll_apth_lock(LOCK) __lll_apth_lock(LOCK, __func__)
#define lll_apth_trylock(LOCK) __lll_apth_trylock(LOCK, __func__)
#define lll_apth_unlock(LOCK) __lll_apth_unlock(LOCK, __func__)
#define lll_internal_lock(LOCK) __lll_internal_lock(LOCK, __func__)
#define lll_internal_unlock(LOCK) __lll_internal_unlock(LOCK, __func__)
#else
#define lll_apth_lock(LOCK) __lll_apth_lock(LOCK)
#define lll_apth_trylock(LOCK) __lll_apth_trylock(LOCK)
#define lll_apth_unlock(LOCK) __lll_apth_unlock(LOCK)
#define lll_internal_lock(LOCK) __lll_internal_lock(LOCK)
#define lll_internal_unlock(LOCK) __lll_internal_unlock(LOCK)
#endif

#endif // __LIBAPTH_UTILS_LLL_NEW_INLINE_H
