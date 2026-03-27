/*
 * apth_numa.c -- NUMA topology public API
 *
 * Provides user-facing functions for querying NUMA topology information:
 *   - apth_get_numa_node_count(): number of NUMA nodes on the system
 *   - apth_get_thread_numa_node(): NUMA node a thread is currently on
 *
 * Only compiled when APTH_NUMA is defined; otherwise the header provides
 * static inline stubs that return sensible defaults.
 */
#ifdef APTH_NUMA

#include "apth.h"
#include "common.h"
#include "internal/types.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_get_numa_node_count(void)
{
    return apth_numa_node_count();
}

int apth_get_thread_numa_node(apth_t th)
{
    if (th == NULL || !APTH_IS_VALID(th))
        return -1;

    /* Dedicated threads bypass the scheduler entirely; they have a
     * dummy scheduler with id == -1 and no meaningful NUMA binding. */
    if (th->is_dedicated)
        return -1;

    apth_sched_t sched = th->current_sched;
    if (sched == NULL || sched->id < 0)
        return -1;

    return sched->numa_node;
}

#endif /* APTH_NUMA */
