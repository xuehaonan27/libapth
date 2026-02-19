#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"

// Enter a cancellation point
APTH_INTERNAL void apth_cancel_point(void)
{
    apth_t cur = cur_apth();
    assert(!APTH_IS_FAKE_SCHED(cur));

    if (cur->cancelreq == true && (cur->cancelhandling & CANCELSTATE_BITMASK) == 0)
    {
        // avoid looping if cleanup handlers contain cancellation points
        cur->cancelreq = false;
        apth_debug("apth_cancel_point: terminating cancelled thread \"%s\"", cur->name);
        // apth_exit(APTH_CANCELED);
        apth_do_cancel(APTH_CANCELED);
    }
    return;
}

// Called when a thread reacts on a cancellation request.
APTH_INTERNAL NORETURN void apth_do_cancel(void *result)
{
    apth_sched_t sched = cur_sched();
    apth_t self = sched->cur;
    apth_debug("apth_do_cancel: cancelling thread \"%s\"", self->name);

    // TODO: atomically set the thread as cancelled.

    // TODO: specially treat main thread

    // Execute cleanups
    apth_thread_cleanup(self);

    // Now mark the current thread as dead, explicitly switch into the scheduler
    // and let it reap the current apth. We cannot free it here.
    {
        self->join_arg = result;
        self->state = APTH_STATE_TERMINATED;
        apth_debug("apth_do_cancel: switching from thread \"%s\" to scheduler", self->name);
        // apth_ctx_switch(self->ctx, sched->sched_ctx);
        apth_yield();
    }

    PANIC("Should not reach here");
}
