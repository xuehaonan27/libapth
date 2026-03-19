#include "common.h" // For MAIN_APTH_EXITED
#include "internal/apth_cancel.h"
#include "internal/types.h"
#include "internal/apth_sched.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"

// Enter a cancellation point
APTH_INTERNAL void apth_cancel_point(void)
{
    apth_t cur = CUR_APTH;
    assert(cur != NULL);

    if (atomic_load_acquire(&cur->cancelreq) && (cur->cancelhandling & CANCELSTATE_BITMASK) == 0)
    {
        // avoid looping if cleanup handlers contain cancellation points
        atomic_store_release(&cur->cancelreq, false);
        apth_debug("terminating cancelled thread \"%s\"", cur->name);
        apth_do_cancel(APTH_CANCELED);
    }
    return;
}

// Called when a thread reacts on a cancellation request.
APTH_INTERNAL void apth_do_cancel(void *result)
{
    apth_sched_t sched = CUR_SCHED;
    apth_t self = sched->cur;
    apth_debug("cancelling thread \"%s\"", self->name);

    // Main apth exited
    if (self == get_MAIN_APTH())
        atomic_store_release(&MAIN_APTH_EXITED, 1);

    // Execute cleanups
    apth_thread_cleanup(self);

    self->join_arg = result;
    // Don't set state here. Scheduler will set it to TERMINATED
    // while holding the terminated queue lock for atomicity.
    apth_debug("switching from thread \"%s\" to scheduler", self->name);
    self->yield_reason = APTH_YIELD_REASON_EXIT;
    apth_yield();

    PANIC("Should not reach here");
}
