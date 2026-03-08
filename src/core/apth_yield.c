#include "apth.h"
#include "internal/apth_tcb.h"
#include "internal/apth_sched.h"
#include "utils/debug.h"

#define APTH_MIN_TIME_SLICE_BITS 18 // 262144 ticks
#define APTH_MIN_TIME_SLICE _BIT(APTH_MIN_TIME_SLICE_BITS)
#define APTH_GET_TIME_SLICE_CNT(t) ((t) >> APTH_MIN_TIME_SLICE_BITS)

int apth_yield(void)
{
    apth_sched_t sched = CUR_SCHED;
    apth_t cur = sched->cur;
    apth_debug("apth_yield: enter from thread \"%s\"", cur->name);

    // TODO: decide a `to` apth and give it a priority

    // Record cpu tick before yielding
    cur->last_yield_tick = cpu_tick();

    apth_debug("apth_yield: give up control to scheduler");
    apth_ctx_switch(CTX(cur), SCHED_CTX(sched));
    apth_debug("apth_yield: got back control from scheduler");
    return 0;
}

int apth_yield_optional(void)
{
    apth_sched_t sched = CUR_SCHED;
    apth_t cur = sched->cur;

    uint64_t cur_tick = cpu_tick();
    uint64_t interval = cur_tick - cur->last_yield_tick;
    int time_slice_in_the_interval = APTH_GET_TIME_SLICE_CNT(interval);
    if (time_slice_in_the_interval >= cur->yield_timeslice)
    {
        cur->yield_reason = APTH_YIELD_REASON_TIMESLICE;
        apth_yield();
    }
    return 0;
}