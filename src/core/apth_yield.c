#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/debug.h"

int apth_yield(void)
{
    apth_sched_t sched = cur_sched();
    apth_t cur = sched->cur;
    apth_debug("apth_yield: enter from thread \"%s\"", cur->name);

    // TODO: decide a `to` apth and give it a priority

    apth_debug("apth_yield: give up control to scheduler");
    apth_ctx_switch(cur->ctx, sched->sched_ctx);
    apth_debug("apth_yield: got back control from scheduler");
    return 0;
}