#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"

APTH_INTERNAL apth_state_t queue_state_of(apth_t th)
{
    return belonging_queue_of(th, "queue_state_of")->th_state;
}

APTH_INTERNAL apth_state_t state_holder_of(apth_t th)
{
    return atomic_load_acquire(&th->state_holder);
}

APTH_INTERNAL void submit_desired_state_to(apth_t th, apth_state_t desired_state, const char *dbg_msg)
{
    assert(state_as_argument_is_valid(desired_state));

    // Previous state should be committed before submitting a new state to `th`
    apth_state_t h = state_holder_of(th);
    assert_msg(state_is_committed(h),
               "previous state should be committed = %d (%s)", h, dbg_msg);

    // Submit current state
    atomic_store_release(&th->state_holder, make_state_uncommitted(desired_state));
}

APTH_INTERNAL void commit_state_of(apth_t th, apth_state_t check)
{
    // th: the apth whose state is to be committed
    // check: check that the state of `th` that's to be committed should
    // match `check`.
    assert(state_as_argument_is_valid(check));

    // Ensure that the caller could only be a scheduler, or is a NEW (apth_create)

    if (check != APTH_STATE_NEW)
    {
        assert_msg(APTH_IS_FAKE_SCHED(cur_apth()), "caller of commit_state_of should be a scheduler");
        assert_msg(APTH_DECODE_FAKE_SCHED(cur_apth()) == cur_sched(), "sanity");
        assert_msg(sched_of(th) == APTH_DECODE_FAKE_SCHED(cur_apth()), "sanity");
    }

    // Ensure that `th` has been inserted to a apth queue before commit its state.
    // And the queue's lock should still be held, which means commit code should:
    // 1. Acquire the queue lock
    // 2. Insert apth to queue
    // 3. Commit state of `th`
    // 4. Release the queue lock
    // (except pushing to waked list, which should be unprotected and private to scheduler)
    // (NOTE: for same reason, work stealing should not touch threads in waked list)
    // Check that `th` has its queue
    assert_msg(belonging_queue_of(th, "commit_state_of") != NULL, "`th` should have belonging queue");

    // Check that the queue's lock is held
    uintptr_t blpkval = lll_peek_val(&belonging_queue_of(th, "lll_peek_val")->th_list_lock);
    assert_msg(blpkval != LLL_NOT_ACQUIRED, "`th` belonging list lock should be acquired");
    // (void)blpkval; // in case of non-debug build, make compiler happy
    // Check that the holder is the caller itself
    assert_msg(
        /* The former situation is for normal case and main apth creation */
        (APTH_IS_FAKE_SCHED(blpkval) && APTH_DECODE_FAKE_SCHED(blpkval) == sched_of(th)) ||
            /* The later situation is for `apth_create` */
            (!APTH_IS_FAKE_SCHED(blpkval) && APTH_DECODE_FAKE_SCHED(blpkval) == cur_apth()),
        "`th` belonging list lock should be acquired by caller");

    // Ensure check
    apth_state_t current_uncommitted_state = atomic_load_acquire(&th->state_holder);
    apth_state_t committed_state = make_state_committed(current_uncommitted_state);
    assert_msg(state_is_uncommitted(current_uncommitted_state), "state should be uncommitted");
    assert_msg(committed_state == check, "state fail to check");

    // Modify the state by CAS. Since the modifier should only be the
    // scheduler that `th` currently belongs to, the CAS should always
    // succeed.

    // The expected value of current state should be uncommitted
    if (!atomic_compare_exchange_strong(
            &th->state_holder,
            &current_uncommitted_state,
            committed_state))
    {
        // Thread state changed unexpectedly, meaning someone else
        // changed it. This is a programming fault.
        PANIC("Apth %p state changed unexpectedly");
        apth_syscall_raw(exit)(127);
    }
}
