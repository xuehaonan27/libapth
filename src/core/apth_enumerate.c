/*
 * apth_enumerate.c -- Thread enumeration and worker count query
 *
 * Provides apth_for_each_thread() for iterating all live threads across
 * all schedulers and dedicated threads, and apth_get_worker_count()
 * for querying the number of worker pthreads.
 */
#include "apth.h"
#include "common.h"
#include "internal/types.h"
#include "internal/apth_global_sched_pool.h"
#include "internal/apth_thqueue.h"
#include "internal/apth_dedicated.h"
#include "utils/lll.inline.h"
#include "utils/list.inline.h"
#include "utils/atomic_wrapper.h"

// Helper: iterate one thqueue, calling visitor for each thread.
// Returns count of threads visited. Sets *stop to true if visitor
// returned non-zero.
static int visit_queue(apth_thqueue_t queue, apth_thread_visitor_t visitor,
                       void *arg, bool *stop)
{
    int count = 0;

    lll_internal_lock(&queue->th_list_lock);
    FOR_ELEMENT_IN_LIST_REF(&queue->th_list, e)
    {
        apth_t th = apth_t_list_entry(e);
        if (!APTH_IS_VALID(th))
            continue;
        count++;
        if (visitor != NULL)
        {
            int rc = visitor(th, arg);
            if (rc != 0)
            {
                *stop = true;
                break;
            }
        }
    }
    lll_internal_unlock(&queue->th_list_lock);

    return count;
}

APTH_API int apth_for_each_thread(apth_thread_visitor_t visitor, void *arg)
{
    int total = 0;
    bool stop = false;

    // 1. Iterate all scheduler queues across all workers
    int nworkers = GLOBAL_POOL.init_worker_count;
    for (int i = 0; i < nworkers && !stop; i++)
    {
        apth_worker_t w = GLOBAL_POOL.worker_ptr_mem_start[i];
        if (w == NULL || w->sched == NULL)
            continue;
        apth_sched_t sched = w->sched;

        // Visit each queue: new, ready, waiting, waked, running, terminated
        total += visit_queue(THQUEUE(sched, new), visitor, arg, &stop);
        if (stop) break;
        total += visit_queue(THQUEUE(sched, ready), visitor, arg, &stop);
        if (stop) break;
        total += visit_queue(THQUEUE(sched, waiting), visitor, arg, &stop);
        if (stop) break;
        total += visit_queue(THQUEUE(sched, waked), visitor, arg, &stop);
        if (stop) break;
        {
            apth_t cur = sched->cur;
            if (cur != APTH_NULL && APTH_IS_VALID(cur))
            {
                total++;
                if (visitor != NULL)
                {
                    int rc = visitor(cur, arg);
                    if (rc != 0) { stop = true; }
                }
            }
        }
        if (stop) break;
        total += visit_queue(THQUEUE(sched, terminated), visitor, arg, &stop);
        if (stop) break;
    }

    // 2. Iterate dedicated thread registry
    if (!stop)
    {
        lll_internal_lock(&__dedicated_registry_lock);
        FOR_ELEMENT_IN_LIST(__dedicated_registry, e)
        {
            apth_t th = list_entry(e, struct apth_st, dedicated_elem);
            if (!APTH_IS_VALID(th))
                continue;
            total++;
            if (visitor != NULL)
            {
                int rc = visitor(th, arg);
                if (rc != 0)
                {
                    lll_internal_unlock(&__dedicated_registry_lock);
                    return total;
                }
            }
        }
        lll_internal_unlock(&__dedicated_registry_lock);
    }

    return total;
}

APTH_API int apth_get_worker_count(void)
{
    return GLOBAL_POOL.init_worker_count;
}
