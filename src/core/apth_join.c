#include "apth.h"
#include "internal/types.h"
#include "internal/apth_event.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include "utils/atomic_wrapper.h"
#include "utils/lll_new.inline.h"

/*
ERRORS
       EDEADLK
              A deadlock was detected (e.g., two threads tried to
              join with each  other);  or  thread  specifies  the
              calling thread.

       EINVAL thread is not a joinable thread.

       EINVAL Another thread is already waiting to join with this
              thread.

       ESRCH  No thread with the ID thread could be found.
*/
// TODO: what will happen if `tid` was switched to DETACHED while cur is waiting its termination?
int apth_join(apth_t tid, void **value)
{
    apth_debug("apth_join: joining thread %p (\"%s\")", tid, tid == NULL ? "-ANY-" : tid->name);
    // apth_sched_t sched = CUR_SCHED;
    apth_t self = CUR_APTH;

    // Trying to join invalid apth
    if (tid == NULL || !APTH_IS_VALID(tid))
        return apth_error(ESRCH, ESRCH);

    // Is the apth joinable?
    if (IS_DETACHED(tid))
        return apth_error(EINVAL, EINVAL);

    // TODO: detected all deadlock situations
    if (tid == self || /* joining myself */
        (self->joinid == tid) /* `tid` is joining me! */)
        return apth_error(EDEADLK, EDEADLK);
    // TODO: should the atomicity semantic be weak or strong?
    // TODO: if the caller (self) is cancelled, `tid` should remain joinable
    else
    {
        apth_t expected = NULL;
        if (apth_unlikely(!atomic_compare_exchange_weak_acquire(&tid->joinid, &expected, self)))
            // There is already somebody waiting for `tid`
            return apth_error(EINVAL, EINVAL);
    }

    // If the `tid` is not terminated, then wait it until so
    if (atomic_load_acquire(&tid->state) != APTH_STATE_TERMINATED)
    {
        struct apth_event_st ev = EVENT_TID(tid, APTH_GOAL_UNTIL_TID_DEAD);
        apth_wait_event(&ev);
    }

    // TODO: if `tid` was switched to DETACHED when we are waiting ...

    apth_debug("tid = %p should have terminated", tid);

    // We mark the `tid` as terminated and as joined
    // NOTE: should get state once again, so state should be volatile.
    // (see `struct apth_st`)
    apth_state_t dbg_tid_state = atomic_load_acquire(&tid->state);
    assert_msg(
        dbg_tid_state == APTH_STATE_TERMINATED,
        "tid = 0x%lx(\"%s\") state = %d", tid, tid->name, dbg_tid_state);

    // Acquire ownership lock to safely access current_sched and current_queue
    lll_internal_lock(&tid->ownership_lock);

    // Get the terminated queue from the APTH's current scheduler
    apth_thqueue_t term_queue = THQUEUE(tid->current_sched, terminated);
    lll_internal_lock(&term_queue->th_list_lock);

    // Check if the thread is still in the terminated queue
    // (it might have been removed by another join attempt)
    if (tid->current_queue != term_queue)
    {
        // Already removed by another thread
        lll_internal_unlock(&term_queue->th_list_lock);
        lll_internal_unlock(&tid->ownership_lock);
        return apth_error(EINVAL, EINVAL);
    }

    // Remove from terminated queue
    list_remove(&tid->elem);
    // atomic_fetch_sub_release(&term_queue->size, 1);
    term_queue->size--;
    tid->current_queue = NULL;

    lll_internal_unlock(&term_queue->th_list_lock);
    lll_internal_unlock(&tid->ownership_lock);

    // Store the return value if the caller is interested
    if (value != NULL)
        *value = tid->join_arg;

    // Note: since the thread is already terminated, then all cleanups should
    // have been executed.
    // Free the TCB
    apth_tcb_free(tid);

    return 0;
}