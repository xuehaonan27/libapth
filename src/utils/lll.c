#include "lll.h"
#include "atomic_wrapper.h"
#include "archplattoold.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "debug.h"

// To make compiler happy
MAYBE_UNUSED static void __dummy_holder__(void *, ...) { /* NOP*/ }

#if defined(APTH_DEBUG) && defined(APTH_DEBUG_LLL)
#ifdef APTH_DEBUG_LLL_USING_FPRINTF
#define lll_debug(s, ...) fprintf(stderr, s "\n", __VA_ARGS__);
#else
#define lll_debug(...) apth_debug(__VA_ARGS__)
#endif // APTH_DEBUG_LLL_USING_FPRINTF
#else
#define lll_debug(...) (void)__dummy_holder__(NULL, __VA_ARGS__)
#endif // APTH_DEBUG && APTH_DEBUG_LLL

APTH_INTERNAL void lll_init(lll_t *lock)
{
    atomic_init(&lock->inner, LLL_NOT_ACQUIRED);
}

APTH_INTERNAL uintptr_t lll_peek_val(lll_t *lock) {
    return atomic_load_acquire(&lock->inner);
}

APTH_INTERNAL void lll_lock(lll_t *lock, const char *from)
{
    lll_debug("entered lll_lock %p (%lx) from=%s", lock, *(uintptr_t *)lock, from);

    apth_sched_t caller_sched = cur_sched();
    apth_worker_t caller_worker = cur_worker();
    apth_t self = caller_sched->cur;

    lll_debug("locking cur=%p, sched=%p, worker=%p", self, caller_sched, caller_worker);
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
        // fprintf(stderr, "%d CONTENTION\n", sched->id);
        lll_debug("%d CONTENTION", caller_sched->id);
        // FIX: Use seq_cst for loading lock value in contended path to ensure
        // proper ordering with ROB operations across different threads
        uintptr_t oldval = atomic_load_explicit(inner, __ATOMIC_SEQ_CST);

        if (oldval == LLL_NOT_ACQUIRED)
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

        assert(self != NULL);
        if (APTH_IS_FAKE_SCHED(self))
        {
            apth_sched_t self_sched = APTH_DECODE_FAKE_SCHED(self);
            assert(self_sched == caller_sched);
            // apth_worker_t self_worker = self_sched->worker;

            // We are now a scheduler, trying to get the lock
            if (APTH_IS_FAKE_SCHED(oldval))
            {
                // The owner is also a scheduler, so spin here, since the owner will release soon.
                apth_sched_t owner = APTH_DECODE_FAKE_SCHED(oldval);
                assert(owner != self_sched);
                sched_yield();
                continue;
            }
            else if (APTH_LLL_IS_ROBBED(oldval))
            {
                // The owner is a robbed apth, meaning the actual holder is
                // a scheduler.
                apth_t owner = APTH_DECODE_FAKE_SCHED(oldval);
                // apth_worker_t owner_worker = owner->worker;
                // apth_sched_t robber = owner_worker->sched;
                apth_sched_t robber = sched_of(owner);

                // if (owner_worker == self_worker)
                if (robber == self_sched)
                {
                    // We are the robber.
                    assert(robber == caller_sched);
                    // NOTE: this is a spurious situation. We are a scheduler and
                    // holding the lock by ourself. LLL is not reentrant lock.
                    PANIC("NOT REENTRANT LLL");
                    apth_syscall_raw(exit)(127);
                }
                else
                {
                    // We are not the robber. We should wait the robber to release the lock.
                    // Not much thing we could do.
                    sched_yield();
                    continue;
                }
            }
            else
            {
                apth_t owner = (apth_t)oldval;
                assert(APTH_IS_VALID(owner));
                // apth_worker_t owner_worker = owner->worker;
                apth_sched_t owner_sched = sched_of(owner);

                // The owner is an apth.
                // TODO: Notice the `owner_worker` to schedule the `owner` as soon as possible!
                // NOTE: For now, we could just wait as well.

                // if (owner_worker == self_worker)
                if (owner_sched == self_sched)
                {
                    // NOTE: this is a hard situation to deal with: the owner is an apth running
                    // on ourself, while holding the lock, its control is yielded. This is totally
                    // possible to happen, but there's no way we could continue, since the apth
                    // running on ourself might requesting our (the scheduler's) help.
                    // NOTE: for now, just ROB the lock held by the apth, and continue.
                    // FIX: Use CAS to ensure atomicity - the lock state might have changed
                    // between our load and this store operation.
                    uintptr_t expected = (uintptr_t)owner;
                    uintptr_t newval = expected | APTH_LLL_ROBBED_MARK_IN_PLACE;
                    if (atomic_compare_exchange_strong(inner, &expected, newval))
                    {
                        return;
                    }
                    // CAS failed, the lock state changed, retry
                    continue;
                }
                else
                {
                    // It's an apth running on another scheduler. Since we are now a scheduler,
                    // there's no way asking another scheduler to schedule the `owner` immediately.
                    // Just have to wait.
                    // TODO: notice `owner_worker` to schedule `owner` as soon as possible.
                    sched_yield();
                    continue;
                }
            }
        }
        else if (APTH_LLL_IS_ROBBED(self))
        {
            // Funny, since ROB value could only occur in value of lll,
            // not in `cur_apth`. It's not possible but we place a check
            // here though.
            PANIC("What???");
            apth_syscall_raw(exit)(127);
        }
        else
        {
            // apth_t self = (apth_t)self;
            assert(APTH_IS_VALID(self));
            // apth_worker_t self_worker = self->worker;
            apth_sched_t self_sched = sched_of(self);

            // We are now an apth, trying to get the lock
            // TODO: check the magic number to determine that this is really an apth?
            if (APTH_IS_FAKE_SCHED(oldval))
            {
                // The owner is a scheduler, so spin here, since the owner will release soon.
                // NOTE: but we could also yield the control to our scheduler here though.

                apth_sched_t owner = APTH_DECODE_FAKE_SCHED(oldval);

                // If the owner is my scheduler, it's considered an error, the scheduler
                // is programmed wrong.
                assert(sched_of(self) != owner);
                sched_yield();
                continue;
            }
            else if (APTH_LLL_IS_ROBBED(oldval))
            {
                // The owner is a robbed apth, meaning the actual holder is
                // a scheduler.
                apth_t owner = APTH_DECODE_FAKE_SCHED(oldval);
                // apth_worker_t owner_worker = owner->worker;
                apth_sched_t robber = sched_of(owner);

                if (robber == self_sched)
                {
                    // We are an apth running on the robber.
                    assert(robber == caller_sched);
                    // NOTE: this is a spurious situation. The scheduler is holding a
                    // lock while letting the control moved to us, an apth!
                    PANIC("SCHEDULER WRONG");
                    apth_syscall_raw(exit)(127);
                }
                else
                {
                    // We are not running on the robber. We should wait the robber to
                    // release the lock. Not much thing we could do.
                    sched_yield();
                    continue;
                }
            }
            else
            {
                // The owner is also an apth. Here we could yield the control back to scheduler.
                // TODO: notice the `owner_worker` to schedule the `owner` as soon as possible.
                apth_t owner = (apth_t)oldval;
                assert(APTH_IS_VALID(owner));
                // apth_worker_t owner_worker = owner->worker;
                apth_sched_t owner_sched = sched_of(owner);

                // if (owner_worker == self_worker)
                if (owner_sched == self_sched)
                {
                    // Yield the control back to scheduler
                    // TODO: tell our scheduler to schedule `owner` as soon as possible
                    apth_yield();
                    continue;
                }
                else
                {
                    // Different worker
                    // TODO: notice `owner_worker` to schedule `owner` as soon as possible
                    apth_yield();
                    continue;
                }
            }
        }
    }
}

APTH_INTERNAL void lll_unlock(lll_t *lock, const char *from)
{
    lll_debug("entered lll_unlock %p (%lx) from=%s", lock, *(uintptr_t *)lock, from);

    assert_msg(*(uintptr_t *)lock != 0, "INSANE");

    apth_sched_t sched = cur_sched();
    apth_worker_t self_worker = cur_worker();
    apth_t self = sched->cur;
    lll_debug("sched=%p, self_worker=%p, self=%p", sched, self_worker, self);

    // fprintf(stderr, "lll_unlock self=%p\n", self);
    _Atomic uintptr_t *inner = &lock->inner;

    // Load the current lock value
    uintptr_t oldval = atomic_load_acquire(inner);

    if (APTH_IS_FAKE_SCHED(self))
    {
        // We are a scheduler
        lll_debug("self=%p", self);
        apth_sched_t decoded_sched = APTH_DECODE_FAKE_SCHED(self);
        assert_msg(decoded_sched == sched, "self=%p, sched=%p", decoded_sched, sched);
        apth_worker_t self_worker = decoded_sched->worker;
        (void)self_worker; // TODO: make compiler happy, may remove this later

        if (APTH_IS_FAKE_SCHED(oldval))
        {
            // The owner is also an scheduler, good.
            apth_sched_t owner = APTH_DECODE_FAKE_SCHED(oldval);
            if (decoded_sched != owner)
            {
                lll_debug("decoded_sched=%p owner=%p", decoded_sched, owner);
                apth_syscall_raw(exit)(127);
            }
            assert(decoded_sched == owner);
            // FIX: Use CAS to ensure atomicity
            uintptr_t expected = oldval;
            if (!atomic_compare_exchange_strong(inner, &expected, LLL_NOT_ACQUIRED))
            {
                // Lock state changed unexpectedly
                PANIC("Scheduler unlock: lock state changed unexpectedly");
                apth_syscall_raw(exit)(127);
            }
        }
        else if (APTH_LLL_IS_ROBBED(oldval))
        {
            // The LLL is robbed.
            apth_t owner = APTH_DECODE_FAKE_SCHED(oldval);
            // apth_worker_t owner_worker = owner->worker;
            // apth_sched_t robber = owner_worker->sched;
            apth_sched_t robber = sched_of(owner);

            assert((apth_sched_t)self == robber);
            // Give the lock back to the apth that should have hold it
            // FIX: Use CAS to ensure atomicity
            uintptr_t expected = oldval;
            if (!atomic_compare_exchange_strong(inner, &expected, (uintptr_t)owner))
            {
                // Lock state changed unexpectedly
                PANIC("Scheduler unlock (robbed): lock state changed unexpectedly");
                apth_syscall_raw(exit)(127);
            }
        }
        else
        {
            // The owner is an apth, but we are unlocker as a scheduler.
            // Programmed wrong.
            PANIC("Fail lll_unlock");
            apth_syscall_raw(exit)(127);
        }
    }
    else if (APTH_LLL_IS_ROBBED(self))
    {
        // Funny, since ROB value could only occur in value of lll,
        // not in `cur_apth`. It's not possible but we place a check
        // here though.
        PANIC("What???");
        apth_syscall_raw(exit)(127);
    }
    else
    {
        // We are an apth.
        assert(APTH_IS_VALID(self));
        if (APTH_IS_FAKE_SCHED(oldval))
        {
            // The owner is also an scheduler.
            // We should not see this. Since scheduler should release its locks
            // before passing control to an apth.
            PANIC("Apth trying to unlock a scheduler's lll");
            apth_syscall_raw(exit)(127);
        }
        else if (APTH_LLL_IS_ROBBED(oldval))
        {
            // The onwer is a robber, meaning a scheduler.
            // But the robber should release its lock before passing control to
            // an apth.
            PANIC("Apth trying to unlock a robber's lll");
            apth_syscall_raw(exit)(127);
        }
        else
        {
            // The owner is also an apth, good.
            apth_t owner = (apth_t)oldval;
            assert(APTH_IS_VALID(owner));
            assert(self == owner);
            // FIX: Use CAS to ensure atomicity
            uintptr_t expected = oldval;
            if (!atomic_compare_exchange_strong(inner, &expected, LLL_NOT_ACQUIRED))
            {
                // Lock state changed unexpectedly
                PANIC("Apth unlock: lock state changed unexpectedly");
                apth_syscall_raw(exit)(127);
            }
        }
    }
}
