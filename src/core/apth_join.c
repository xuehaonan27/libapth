#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include "utils/atomic_wrapper.h"
#include "utils/archplattoold.h"

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
    apth_event_t ev;

    apth_debug("apth_join: joining thread %p (\"%s\")", tid, tid == NULL ? "-ANY-" : tid->name);
    apth_sched_t sched = cur_sched();
    apth_t self = sched->cur;

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
    else if (apth_unlikely(atomic_compare_exchange_weak_acquire(&tid->joinid, &self, NULL)))
        // There is already somebody waiting for `tid`
        return apth_error(EINVAL, EINVAL);

    // If the `tid` is not terminated, then wait it until so
    if (tid->state != APTH_STATE_TERMINATED)
    {
        ev = apth_event_tid(APTH_GOAL_UNTIL_TID_DEAD | APTH_EVENT_MODE_STATIC, tid);
        apth_wait_event(ev);
    }

    // TODO: if `tid` was switched to DETACHED when we are waiting ...

    apth_debug("(%d) tid = %p should have terminated", tid);

    // We mark the `tid` as terminated and as joined
    assert_msg(tid->state == APTH_STATE_TERMINATED,
               "tid = 0x%lx(\"%s\") state = %d", tid, tid->name, tid->state);

    // Store the return value if the caller is interested
    if (value != NULL)
        *value = tid->join_arg;

    // Remove the thread from scheduler. This is sane because scheduler itself
    // would do nothing about threads in the terminated list. All of them are
    // free for other threads to join to.
    // But the apth could has not been transferred to terminated list yet.
    wait_apth_to_be_in_list(tid);
    remove_apth(tid);
    // Note: since the thread is already terminated, then all cleanups should
    // have been executed.
    // Free the TCB
    apth_tcb_free(tid);

    return 0;
}