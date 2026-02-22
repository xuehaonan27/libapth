#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_errno.h"
#include "utils/apth_sysutils.h"
#include "utils/atomic_wrapper.h"
#include "utils/debug.h"
#include <stdlib.h>
#include <string.h>

APTH_INTERNAL int check_stacksize_attr(size_t st)
{
    if (st >= APTH_STACK_SIZE_DEFAULT)
        return 0;

    return EINVAL;
}

// The most lower 2 bits should be 0, meaning TCB should be at least 4 bytes aligned.
#define IS_VALID_APTH_T(t) (((uintptr_t)(t) & 0x3) == 0)

APTH_INTERNAL apth_t apth_tcb_alloc(size_t stacksize, void *stackaddr)
{
    apth_debug("enter");
    apth_t t;

    if (stacksize > 0 && stacksize < APTH_STACK_SIZE_DEFAULT)
        stacksize = APTH_STACK_SIZE_DEFAULT;
    if ((t = (apth_t)malloc(sizeof(struct apth_st))) == NULL)
        return APTH_NULL;

    assert_msg(IS_VALID_APTH_T(t), "t not aligned to 4 bytes: %p", t);

    memset(t, '\0', sizeof(struct apth_st));

    t->ctx = apth_ctx_alloc();

    t->stacksize = stacksize;
    t->stack_mem_start = NULL;
    t->stackguard = NULL;
    t->stackloan = (stackaddr != NULL ? true : false);
    if (stacksize > 0)
    {
        if (stackaddr != NULL)
        // t->stack_mem_start = (char *)stackaddr;
#if APTH_STACKGROWTH < 0
            t->stack_mem_start = (char *)stackaddr - stacksize;
#else
            t->stack_mem_start = (char *)stackaddr;
#endif
        else
        {
            if ((t->stack_mem_start = (char *)malloc(stacksize)) == NULL)
            {
                apth_shield
                {
                    free(t->ctx);
                    free(t);
                }
                return APTH_NULL;
            }
        }

        // TODO: set guard region, allocating extra space at the end of the stack.
        // TODO: A guard area consists of virtual memory pages that are protected to prevent read and write access.
#if APTH_STACKGROWTH < 0
        /* guard is at lowest address (alignment is guaranteed) */
        t->stackguard = (uint32_t *)(t->stack_mem_start); /* double cast to avoid alignment warning */
#else
        /* guard is at highest address (be careful with alignment) */
        t->stackguard = (uint32_t *)(t->stack_mem_start + (((stacksize / sizeof(long)) - 1) * sizeof(long)));
#endif
        *(uint32_t *)(t->stackguard) = APTH_MAGIC;
    }

    list_init(&t->event_list);
    // TODO: initialize fields

    apth_debug("leave");
    return t;
}

APTH_INTERNAL void apth_tcb_free(apth_t t)
{
    assert(t != NULL);
    apth_sched_t sched = cur_sched();

    if (t->stack_mem_start != NULL && !t->stackloan)
        free(t->stack_mem_start);

    // apth_thread_cleanup(t);

    assert_msg(t->cleanups == NULL, "apth %p try to TCB free without executing cleanups", t);

    // TODO: Clear other fields

    free(t->ctx);
    free(t);

    // Decrement
    dec_thrcnt(sched);

    return;
}

APTH_INTERNAL void submit_desired_state_to(apth_t th, apth_state_t desired_state)
{
    assert(state_as_argument_is_valid(desired_state));

    // Previous state should be committed before submitting a new state to `th`
    assert_msg(
        state_is_committed(atomic_load_acquire(&th->state_holder)),
        "previous state should be committed");

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
    // (except pushing to waked list, which should be unprotected and private to scheudler)
    // (NOTE: for same reason, work stealing should not touch threads in waked list)
    // TODO: check that `th` has its queue
    assert_msg(belonging_list_of(th) != NULL, "`th` should have belonging list");
    // TODO: check that the queue's lock is held
    uintptr_t blpkval = lll_peek_val(belonging_list_lock_of(th));
    assert_msg(blpkval != LLL_NOT_ACQUIRED, "`th` belonging list lock should be acquired");
    (void)blpkval; // in case of non-debug build, make compiler happy
    // TODO: check that the holder is the caller itself
    assert_msg(
        /* The former situation is for normal case and main apth creation */
        (APTH_IS_FAKE_SCHED(blpkval) && APTH_DECODE_FAKE_SCHED(blpkval) == (uintptr_t)sched_of(th)) ||
            /* The later situation is for `apth_create` */
            (!APTH_IS_FAKE_SCHED(blpkval) && APTH_DECODE_FAKE_SCHED(blpkval) == (uintptr_t)cur_apth()),
        "`th` belonging list lock should be acquired by caller");

    // And for future apth queue implementation:
    // TODO: queue should have a thread-safe method doing 1,2,3,4 atomically.

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

// `raw` means that the returned state might be uncommitted (meaning with invalid lower 1 bit set)
APTH_INTERNAL apth_state_t raw_state_of(apth_t th)
{
    return atomic_load_acquire(&th->state_holder);
}

APTH_INTERNAL apth_sched_t sched_of(apth_t th)
{
    return atomic_load_acquire(&th->belongs_to_sched);
}

// TODO: should remove this
APTH_INTERNAL void set_sched_of(apth_t th, apth_sched_t sched)
{
    atomic_store_release(&th->belongs_to_sched, sched);
}

APTH_INTERNAL struct list *belonging_list_of(apth_t th)
{
    return atomic_load_acquire(&th->belongs_to_list);
}

APTH_INTERNAL void set_belonging_list_of(apth_t th, struct list *l)
{
    atomic_store_release(&th->belongs_to_list, l);
}

APTH_INTERNAL lll_t *belonging_list_lock_of(apth_t th)
{
    return atomic_load_acquire(&th->belongs_to_list_lock);
}

// Should remove this
APTH_INTERNAL void set_belonging_list_lock_of(apth_t th, lll_t *l)
{
    atomic_store_release(&th->belongs_to_list_lock, l);
}