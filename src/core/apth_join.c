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
int apth_join(apth_t tid, void **value)
{
    apth_event_t ev;

    apth_debug("apth_join: joining thread \"%s\"", tid == NULL ? "-ANY-" : tid->name);
    apth_sched_t sched = cur_sched();
    apth_t self = sched->cur;

    // TODO: determine that this tid is valid
    if (tid == APTH_NULL || apth_is_not_null_and_valid(tid))
        return apth_error(ESRCH, ESRCH);
    // Is the apth joinable?
    if (tid != NULL && IS_DETACHED(tid))
        return apth_error(EINVAL, EINVAL);

    // TODO: detected all deadlock situations
    if (tid == self || /* joining myself */
        (self->joinid == tid) /* `tid` is joining me! */)
        return apth_error(EDEADLK, EDEADLK);
    else if (apth_unlikely(atomic_compare_exchange_weak_acquire(&tid->joinid, &self, NULL)))
        // There is already somebody waiting for `tid`
        return apth_error(EINVAL, EINVAL);

    // If the `tid` is not terminated, then wait it until so
    if (tid->state != APTH_STATE_TERMINATED)
    {
        ev = apth_event_tid(APTH_GOAL_UNTIL_TID_DEAD | APTH_EVENT_MODE_STATIC, tid);
        apth_wait_event(ev);
    }

    // We mark the `tid` as terminated and as joined
    if (tid->state != APTH_STATE_TERMINATED)
    {
        apth_debug("apth_join: tid = 0x%lx(\"%s\") state = %d", tid, tid->name, tid->state);
        return apth_error(EINVAL, EINVAL);
    }
    // Store the return value if the caller is interested
    if (value != NULL)
        *value = tid->join_arg;
    // Remove the thread from scheduler
    // TODO: lock up the list
    list_remove(&tid->elem);
    // Free the TCB
    apth_tcb_free(tid);

    return 0;
}