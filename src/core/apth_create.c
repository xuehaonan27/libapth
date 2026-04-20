#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>

#include "common.h" // For APTH_TCB_NAMELEN
#include "internal/apth_global_sched_pool.h"
#include "internal/apth_tcb.h"
#include "internal/apth_cancel.h"
#include "internal/apth_dedicated.h"
#include "internal/apth_sched.h"
#include "internal/apth_thqueue.h"
#include "internal/apth_stats.h"
#include "attr/apth_attr.h"
#include "hook_libc/hooked_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/lll.inline.h"
#include "utils/apth_errno.h"

static void apth_create_trampoline(void)
{
    void *data;
    // If this apth is scheduled to run, then `cur` should point to current apth
    apth_t cur = CUR_APTH;

    data = (*cur->start_func)(cur->start_arg);

    // Note: we cannot call `apth_exit` here, since that should be called optionally
    // and explicitly by program, marking the process not to exit with the main apth.
    apth_do_cancel(data);
    PANIC("Should not reach here");
}

APTH_API int apth_create(apth_t *newthr, const apth_attr_t *attr,
                         void *(*start_routine)(void *), void *arg)
{
    apth_t t;
    apth_attr_t default_attr;
    const struct apth_attr_st *iattr;
    apth_time_t ts;
    // apth_t cur;
    apth_sched_t sched = NULL;
    apth_worker_t worker = NULL;

    // Consistency
    if (start_routine == NULL)
        return apth_error(EINVAL, EINVAL);

    if (attr != NULL)
    {
        iattr = APTH_ATTR_CONST_CAST(attr);
    }
    else
    {
        apth_attr_init(&default_attr);
        iattr = APTH_ATTR_CONST_CAST(&default_attr);
    }

    {
        // Lazy-start: if this is a non-DEDICATED thread, ensure the scheduler
        // pool and reactor are started (deferred from apth_init for performance).
        extern bool __apth_scheduler_pool_started;
        extern int apth_ensure_scheduler_pool(void);
        if (iattr->thread_class != APTH_CLASS_DEDICATED && !__apth_scheduler_pool_started) {
            if (apth_ensure_scheduler_pool() != 0)
                return apth_error(EAGAIN, EAGAIN);
        }

        // We are spawning other threads. TLS should have been set. The scheduler
        // to spawn to new apth to, should be determined first from CPU affinity.
        // If no affinity is specified, then spawn in current scheduler.
        int cpu_favored = -1;
        if (iattr->cpuset != NULL)
        {
            // From CPU affinity
            size_t check_cpu_cnt =
                iattr->cpusetsize < (size_t)GLOBAL_POOL.worker_count ? iattr->cpusetsize : (size_t)GLOBAL_POOL.worker_count;

            for (size_t cnt = 0; cnt < check_cpu_cnt; cnt++)
            {
                if (CPU_ISSET(cnt, iattr->cpuset))
                {
                    // pick this cpu
                    cpu_favored = cnt;
                    break;
                }
            }
        }

        // Determine the sched
        if (cpu_favored != -1)
        {
            worker = get_worker_by_id(cpu_favored);
            sched = worker->sched;
        }
        else
        {
#ifdef APTH_NUMA
            // NUMA-aware round-robin: distribute new threads across
            // schedulers on the same NUMA node as the creating scheduler.
            // This avoids piling all threads onto the creating scheduler
            // while keeping them NUMA-local.
            apth_sched_t cur = CUR_SCHED;
            assert(cur != NULL);
            if (cur->id >= 0)
            {
                static _Atomic(unsigned int) numa_rr_counter = 0;
                unsigned int rr = atomic_fetch_add_relaxed(
                    &numa_rr_counter, 1);
                int n_workers = GLOBAL_POOL.init_worker_count;
                int my_node = cur->numa_node;
                apth_sched_t best = cur; // fallback: current scheduler

                for (int i = 0; i < n_workers; i++)
                {
                    int cand_id = (int)((rr + (unsigned int)i) % (unsigned int)n_workers);
                    apth_worker_t cand_w = GLOBAL_POOL.worker_ptr_mem_start[cand_id];
                    if (cand_w == NULL || cand_w->sched == NULL)
                        continue;
                    if (!apth_sched_is_opening(cand_w->sched))
                        continue;
                    if (cand_w->sched->numa_node == my_node)
                    {
                        best = cand_w->sched;
                        break;
                    }
                }
                sched = best;
            }
            else
            {
                // Dummy scheduler (dedicated thread context), pick any
                sched = cur;
            }
#else
            // Spawn in current scheduler
            sched = CUR_SCHED;
            assert(sched != NULL);
#endif /* APTH_NUMA */
        }
    }

    assert(sched != NULL);

    // If called from a dedicated thread context, CUR_SCHED is a dummy scheduler.
    // Pick a real scheduler via round-robin to spread M:N threads across workers.
    // With lazy-start, the pool may not be started yet — keep the dummy scheduler.
    if (sched->id < 0 && GLOBAL_POOL.init_worker_count > 0)
    {
        static _Atomic(unsigned int) __dedicated_rr = 0;
        unsigned int idx = atomic_fetch_add_relaxed(&__dedicated_rr, 1);
        unsigned int nworkers = (unsigned int)GLOBAL_POOL.init_worker_count;
        sched = GLOBAL_POOL.worker_ptr_mem_start[idx % nworkers]->sched;
    }

    // APTH_CLASS_DISTRIBUTED: round-robin across all schedulers to spread
    // threads evenly (e.g., GC workers in JVM). Overrides affinity/NUMA choice.
    if (iattr->thread_class == APTH_CLASS_DISTRIBUTED && GLOBAL_POOL.init_worker_count > 0)
    {
        static _Atomic(unsigned int) __rr_counter = 0;
        unsigned int idx = atomic_fetch_add_relaxed(&__rr_counter, 1);
        int sched_id = (int)(idx % (unsigned int)GLOBAL_POOL.init_worker_count);
        apth_worker_t rr_worker = GLOBAL_POOL.worker_ptr_mem_start[sched_id];
        if (rr_worker != NULL && rr_worker->sched != NULL)
            sched = rr_worker->sched;
    }

    // ==================== Dedicated thread creation path ====================
    if (iattr->thread_class == APTH_CLASS_DEDICATED)
    {
        APTH_STAT_INC(__apth_stats_dedicated_creates);
        // Allocate TCB only (no mmap stack, no context setup — the pthread has its own stack)
        if ((t = (apth_t)malloc(sizeof(struct apth_st))) == NULL)
            return apth_error(ENOMEM, ENOMEM);
        memset(t, '\0', sizeof(struct apth_st));

        t->magic = APTH_MAGIC;
        t->stack_mem_start = NULL;
        t->stackloan = true; // Prevent stack pool return in apth_tcb_free
        t->stacksize = 0;
        t->guardsize = 0;
        list_init(&t->event_list);

        // Standard TCB fields
        t->prio = iattr->schedparam.sched_priority;
        t->thread_class = APTH_CLASS_DEDICATED;
        memcpy(t->name, iattr->name, APTH_TCB_NAMELEN);
        t->name[APTH_TCB_NAMELEN] = '\0';
        t->dispatches = 0;
        atomic_store_release(&t->state, APTH_STATE_NEW);

        apth_time_t ts;
        apth_time_set(&ts, APTH_TIME_NOW);
        apth_time_set(&t->spawned, &ts);
        apth_time_set(&t->lastran, &ts);
        apth_time_set(&t->running, APTH_TIME_ZERO);

        t->wake_pending = false;
        sigemptyset(&t->sigpending);
        t->sigpendcnt = 0;
        lll_internal_init(&t->siglock);
        t->in_sighandler = false;
        t->sigaltstack_set = false;

        // Signal mask
        if (iattr->sigmask_set)
            t->sigmask = iattr->sigmask;
        else
        {
            apth_t creator = CUR_APTH;
            if (creator != NULL)
                t->sigmask = creator->sigmask;
            else
                sigemptyset(&t->sigmask);
        }

        // Ownership
        t->home_sched = sched;
        t->current_queue = NULL;
        lll_internal_init(&t->ownership_lock);

        // Start routine
        t->start_func = start_routine;
        t->start_arg = arg;

        // Join
        t->join_arg = NULL;
        t->joinid = iattr->flags & ATTR_FLAG_DETACHSTATE ? t : NULL;

        // Cancellation
        atomic_store_release(&t->cancelreq, false);
        t->cancelhandling = 0;
        t->cleanups = NULL;

        // TLS
        t->specific[0] = t->specific_1stblock;
        t->specific_used = false;

        // Yield
        t->last_yield_tick = cpu_tick();
        t->yield_timeslice = 10;
        t->yield_reason = APTH_YIELD_REASON_VOLUNTEER;

        // Yield hooks (not used for dedicated threads, but initialize for safety)
        t->pre_yield_hook = NULL;
        t->post_resume_hook = NULL;
        t->yield_hook_arg = NULL;

        // Dedicated thread fields
        t->is_dedicated = true;
        t->dedicated_futex_val = 0;  // futex: 0=sleeping, 1=woken
        t->dedicated_wake_fd = -1;   // eventfd no longer needed (using futex)
        // Do NOT register wake_fd with LIBAPTH FD table

        // Allocate dummy scheduler for TLS compatibility
        t->dedicated_dummy_sched = (apth_sched_t)calloc(1, sizeof(struct apth_sched_st));
        if (t->dedicated_dummy_sched == NULL)
        {
            apth_func_raw(close)(t->dedicated_wake_fd);
            free(t);
            return apth_error(ENOMEM, ENOMEM);
        }
        t->dedicated_dummy_sched->id = -1; // Mark as dummy
        t->dedicated_dummy_sched->wake_eventfd = -1;
        t->dedicated_dummy_sched->epoll_fd = -1;
        t->current_sched = t->dedicated_dummy_sched;

        // Spawn the dedicated pthread
        pthread_attr_t pattr;
        apth_func_raw(pthread_attr_init)(&pattr);

        // Set CPU affinity if specified
        if (iattr->cpuset != NULL)
            apth_func_raw(pthread_attr_setaffinity_np)(&pattr, iattr->cpusetsize, iattr->cpuset);

        int ret = apth_func_raw(pthread_create)(&t->dedicated_tid, &pattr,
                                                 apth_dedicated_thread_wrapper, t);
        pthread_attr_destroy(&pattr);

        if (ret != 0)
        {
            apth_func_raw(close)(t->dedicated_wake_fd);
            free(t->dedicated_dummy_sched);
            free(t);
            return apth_error(ret, ret);
        }

        // Increment global counters (not per-scheduler — dedicated has no scheduler)
        atomic_fetch_add_release(&apth_nthreads, 1);
        inc_alive_thrcnt();

        apth_debug("created dedicated thread %p (\"%s\")", t, t->name);
        *newthr = t;
        return 0;
    }

    // ==================== Regular apth creation path ====================

    // Allocate a new thread control block, do all the allocations

    void *stackaddr = iattr->flags & ATTR_FLAG_STACKADDR ? iattr->stackaddr : NULL;
    if ((t = apth_tcb_alloc(iattr->stacksize, stackaddr, iattr->guardsize)) == NULL)
        return apth_error(errno, errno);

    // Standard TCB ingredients
    t->prio = iattr->schedparam.sched_priority;
    t->thread_class = iattr->thread_class;
    memcpy(t->name, iattr->name, APTH_TCB_NAMELEN);
    t->name[APTH_TCB_NAMELEN] = '\0';
    t->dispatches = 0;
    atomic_store_release(&t->state, APTH_STATE_NEW);

    // Timing: initialize the time points and ranges
    apth_time_set(&ts, APTH_TIME_NOW);
    apth_time_set(&t->spawned, &ts);
    apth_time_set(&t->lastran, &ts);
    apth_time_set(&t->running, APTH_TIME_ZERO);

    // Events
    list_init(&t->event_list);
    t->wake_pending = false;

    // Signals
    sigemptyset(&t->sigpending);
    t->sigpendcnt = 0;
    lll_internal_init(&t->siglock);
    t->in_sighandler = false;
    t->sigaltstack_set = false;

    // Signal mask: inherit creator's mask or use attribute designated one
    if (iattr->sigmask_set)
        t->sigmask = iattr->sigmask;
    else
    {
        apth_t creator = CUR_APTH;
        if (creator != NULL)
            t->sigmask = creator->sigmask;
        else
            sigemptyset(&t->sigmask);
    }

    // Ownership system: initialize scheduler ownership fields
    t->home_sched = sched;    // Immutable: set at creation
    t->current_sched = sched; // Initially owned by creating scheduler
    t->current_queue = NULL;  // Will be set when pushed to queue
    lll_internal_init(&t->ownership_lock);

    // Remember the start routine and arguments
    t->start_func = start_routine;
    t->start_arg = arg;

    // Initialize join argument
    t->join_arg = NULL;
    t->joinid = iattr->flags & ATTR_FLAG_DETACHSTATE ? t : NULL;

    // Initialize cancellation stuff
    atomic_store_release(&t->cancelreq, false);
    t->cancelhandling = 0; // TODO: is this right? we should only clear enable bit
    t->cleanups = NULL;

    // TODO: exception stuff

    // Initialize thread specific storage
    t->specific[0] = t->specific_1stblock;
    t->specific_used = false;

    // Yield information
    t->last_yield_tick = cpu_tick();
    t->yield_timeslice = 10; // TODO: should passed from attr
    t->yield_reason = APTH_YIELD_REASON_VOLUNTEER;

    // Yield hooks (set later via apth_set_yield_hooks)
    t->pre_yield_hook = NULL;
    t->post_resume_hook = NULL;
    t->yield_hook_arg = NULL;

    // Initialize the machine context of this new thread
    assert_msg(t->stacksize > 0, "APTH 0x%lx have stack size <= 0", t);

    // Get the usable stack start (after guard page if present)
    char *usable_stack_start = apth_tcb_get_usable_stack_start(t);
    if (!apth_ctx_set(CTX(t), apth_create_trampoline, usable_stack_start, t->stacksize))
    {
        apth_shield
        {
            apth_tcb_free(t);
        }
        return apth_error(EINVAL, EINVAL);
    }

    // Finally insert it into the new queue where
    // the scheduler will pick it up for dispatching
    // push_apth_to_new(t, sched);
    push_apth_to(THQUEUE(sched, new), t);

    // Wake the scheduler in case it's blocked in epoll_wait waiting for work
    apth_sched_wake(sched);

    // Increment scheduler thread count
    inc_thrcnt(sched);
    inc_alive_thrcnt();

    apth_debug("Now alive thread count = %u", get_apth_alive_nthreads());

    *newthr = t;
    return 0;
}
