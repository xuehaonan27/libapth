#ifndef __LIBAPTH_UTILS_LLL_NEW_INLINE_H
#define __LIBAPTH_UTILS_LLL_NEW_INLINE_H

#include "common.h"
#include "apth.h"
#include <stdio.h>
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

    // During shutdown or in scheduler context, CUR_APTH may be NULL.
    // Use a sentinel value so ownership tracking still works and the
    // spinlock degrades to a pause loop (no yield possible without an apth).
    bool no_apth = (self == NULL);
    if (no_apth)
        self = (apth_t)(uintptr_t)0x1; // non-NULL sentinel

    // Fast path: try to acquire
    apth_t expected = NULL;
    if (atomic_compare_exchange_acquire(&lock->owner, &expected, self))
        return;

    // Slow path: spin-pause first, then yield if still contended
    int __total_spins = 0;
    while (1)
    {
        for (int __spin = 0; __spin < 32; __spin++)
        {
            apth_t owner = atomic_load_acquire(&lock->owner);
            if (owner == NULL)
            {
                expected = NULL;
                if (atomic_compare_exchange_acquire(&lock->owner, &expected, self))
                    return;
            }
            __builtin_ia32_pause();
        }

        __total_spins += 32;
        if (__total_spins > 10000000)
        {
            __total_spins = 0;
            fprintf(stderr, "[LIBAPTH] lll_apth_lock spin: lock=%p owner=%p self=%p no_apth=%d\n",
                    (void *)lock, (void *)atomic_load_acquire(&lock->owner), (void *)self, no_apth);
        }

        if (no_apth)
            __builtin_ia32_pause();
        else
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
    if (self == NULL)
        self = (apth_t)(uintptr_t)0x1;

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
    if (self == NULL)
        self = (apth_t)(uintptr_t)0x1;
    apth_t expected = self;

    if (!atomic_compare_exchange_release(&lock->owner, &expected, NULL))
    {
        // During shutdown, a different context may have acquired the lock
        // with the sentinel.  Force-release rather than panicking.
        atomic_store_release(&lock->owner, NULL);
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
    int spin_count = 0;
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
            // Scheduler: spin with pause instruction (no syscall).
            // Critical sections protected by internal locks are short,
            // so a brief spin is cheaper than yielding the whole CPU core.
            if (spin_count < 64)
            {
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#else
                __asm__ volatile("" ::: "memory");
#endif
                spin_count++;
            }
            else
            {
                // After many spins, yield to avoid live-lock
                sched_yield();
                spin_count = 0;
            }
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
