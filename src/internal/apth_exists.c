#include "internal_funcs.h"
#include "internal_types.h"

int apth_apth_exists(apth_t t)
{
    if (!APTH_IS_VALID(t))
        return false;

    apth_sched_t sched = cur_sched();

    bool found_in_new = apth_is_in(sched->new_queue, t);
    bool found_in_ready = apth_is_in(sched->ready_queue, t);
    bool found_in_waiting = apth_is_in(sched->waiting_queue, t);
    bool found_in_terminated = apth_is_in(sched->terminated_queue, t);
    bool found_in_waked = apth_is_in(sched->waked_queue, t);

    return found_in_new || found_in_ready || found_in_waiting || found_in_terminated || found_in_waked;
}
