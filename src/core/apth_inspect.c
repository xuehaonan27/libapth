/*
 * apth_inspect.c -- Thread state query and stack inspection APIs
 *
 * These functions allow external code (e.g., JVM GC, safepoint mechanism)
 * to inspect thread state, retrieve saved stack pointers for stack walking,
 * and query thread statistics.
 */
#include "apth.h"
#include "common.h"
#include "internal/types.h"
#include "internal/apth_time.h"
#include "utils/apth_errno.h"
#include "utils/atomic_wrapper.h"

int apth_getstate(apth_t th)
{
    if (th == NULL || !APTH_IS_VALID(th))
        return apth_error(-1, ESRCH);
    return (int)atomic_load_acquire(&th->state);
}

int apth_get_saved_sp(apth_t th, void **sp_out)
{
    if (th == NULL || !APTH_IS_VALID(th))
        return ESRCH;
    if (sp_out == NULL)
        return EINVAL;
    if (th->is_dedicated)
        return ENOTSUP;
    if (atomic_load_acquire(&th->state) == APTH_STATE_RUNNING)
        return EBUSY;
    *sp_out = th->ctx_st.sp;
    return 0;
}

int apth_get_stack_bounds(apth_t th, void **base_out, size_t *size_out)
{
    if (th == NULL || !APTH_IS_VALID(th))
        return ESRCH;
    if (th->is_dedicated)
        return ENOTSUP;
    if (base_out != NULL)
    {
        /* Usable stack starts after guard page */
        if (th->guardsize > 0)
            *base_out = (void *)(th->stack_mem_start + th->guardsize);
        else
            *base_out = (void *)th->stack_mem_start;
    }
    if (size_out != NULL)
        *size_out = th->stacksize;
    return 0;
}

int apth_get_thread_stats(apth_t th, struct apth_thread_stats *stats)
{
    if (th == NULL || !APTH_IS_VALID(th))
        return ESRCH;
    if (stats == NULL)
        return EINVAL;
    stats->dispatches = th->dispatches;
    if (th->accumulated_cpu_ns > 0)
        stats->cpu_time_sec = (double)th->accumulated_cpu_ns / 1e9;
    else
        stats->cpu_time_sec = apth_time_t2d(&th->running);
    apth_time_t now, elapsed;
    apth_time_set(&now, APTH_TIME_NOW);
    apth_time_set(&elapsed, &now);
    apth_time_sub(&elapsed, &th->spawned);
    stats->wall_time_sec = apth_time_t2d(&elapsed);
    stats->thread_class = th->thread_class;
    stats->state = (int)atomic_load_acquire(&th->state);
    return 0;
}
