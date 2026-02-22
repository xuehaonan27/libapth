#include "internal_funcs.h"
#include "internal_types.h"

int apth_apth_exists(apth_t t)
{
    apth_sched_t sched = cur_sched();
    
    bool found = apth_is_in_new(t, sched) ? true :
        (apth_is_in_new(t, sched) ? true :
        (apth_is_in_ready(t, sched) ? true :
        (apth_is_in_waiting(t, sched) ? true :
        (apth_is_in_terminated(t, sched)))));

    return found;
}
