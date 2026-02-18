#include "lll.h"
#include "atomic_wrapper.h"
#include "archplattoold.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "debug.h"

APTH_INTERNAL void lll_init(lll_t *lock)
{
    atomic_init(&lock->inner, LLL_NOT_ACQUIRED);
}

// #if defined(APTH_DEBUG) && defined(APTH_DEBUG_LLL)
APTH_INTERNAL void lll_lock(lll_t *lock, const char *from)
{
#if defined(APTH_DEBUG) && defined(APTH_DEBUG_LLL)
    fprintf(stderr, "entered lll_lock %p (%lx) from=%s\n", lock, *(uintptr_t *)lock, from);
#endif
    // #else  // APTH_DEBUG_LLL
    // APTH_INTERNAL void lll_lock(lll_t *lock)
    // {
    // #endif // APTH_DEBUG_LLL
    apth_sched_t sched = cur_sched();
    apth_worker_t self_worker = cur_worker();
    apth_t self = sched->cur;
    // apth_worker_t self_worker = self->worker;

#if defined(APTH_DEBUG) && defined(APTH_DEBUG_LLL)
    fprintf(stderr, "locking cur=%p, sched=%p, worker=%p\n", self, sched, self_worker);
#else
    (void)from; // Make compiler happy
#endif
    // fprintf(stderr, "locking cur=%p, sched=%p\n", self, sched);

    assert(self != NULL);

    _Atomic uintptr_t *inner = &lock->inner;
    uintptr_t self_ptr = (uintptr_t)self;

    // Fast path: try to acquire the lock if it's not held
    uintptr_t expected = LLL_NOT_ACQUIRED;

    if (atomic_compare_exchange_acquire(inner, &expected, self_ptr))
    {
        // Successfully acquired the lock
        return;
    }

    // Slow path: lock is contended
    while (1)
    {
        uintptr_t oldval = atomic_load_acquire(inner);
        apth_t owner = (apth_t)oldval;

        if (owner == NULL)
        {
            // Lock is free, try to acquire it
            expected = LLL_NOT_ACQUIRED;

            if (atomic_compare_exchange_acquire(inner, &expected, self_ptr))
            {
                // Successfully acquired the lock
                return;
            }
            // Failed to acquire, retry
            continue;
        }

        // Lock is held by someone else
        // Determine if the owner is on the same worker as us
        apth_worker_t owner_worker = owner->worker;
        // assert(owner_worker != NULL);

        // Decide whether to yield based on whether we're on the same worker
        if (APTH_IS_FAKE_SCHED(owner) || owner_worker == self_worker)
        {
            // Same worker: we MUST yield to avoid deadlock
            // The owner needs CPU time to release the lock
            apth_yield();
        }
        else
        {
            // Different worker: the owner is running on another CPU
            // We could spin a bit before yielding for better performance
            // For now, yield immediately to minimize overhead
            apth_yield();
        }

        // After yielding, retry acquiring the lock
    }
}

// #if defined(APTH_DEBUG) && defined(APTH_DEBUG_LLL)
APTH_INTERNAL void lll_unlock(lll_t *lock, const char *from)
{
#if defined(APTH_DEBUG) && defined(APTH_DEBUG_LLL)
    fprintf(stderr, "entered lll_unlock %p (%lx) from=%s\n", lock, *(uintptr_t *)lock, from);
#else
    (void)from; // Make compiler happy
#endif
    // #else  // APTH_DEBUG_LLL
    // APTH_INTERNAL void lll_unlock(lll_t *lock)
    // {
    // #endif // APTH_DEBUG_LLL
    if (*(uintptr_t *)lock == 0)
    {
        fprintf(stderr, "INSANE\n");
        apth_syscall_raw(exit)(127);
    }
    apth_t self = cur_apth();
    // fprintf(stderr, "lll_unlock self=%p\n", self);
    _Atomic uintptr_t *inner = &lock->inner;

    // Load the current lock value
    uintptr_t oldval = atomic_load_acquire(inner);
    apth_t owner = (apth_t)oldval;

    // Verify that we own this lock
    // In production code, this might be a debug-only check
    assert_msg(owner == self, "lock=%p, owner=%p, self=%p", lock, owner, self);
    // if (owner != self) {
    //     fprintf(stderr,  "lock=%p, owner=%p, self=%p from=%s", lock, owner, self, from);
    //     apth_syscall_raw(exit)(127);
    // }

    // Release the lock by storing 0
    // Use release semantics to ensure all previous writes are visible
    // to the next thread that acquires the lock
    atomic_store_release(inner, LLL_NOT_ACQUIRED);
}
