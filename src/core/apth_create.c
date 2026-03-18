#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>

#include "common.h" // For APTH_TCB_NAMELEN
#include "internal/apth_global_sched_pool.h"
#include "internal/apth_tcb.h"
#include "internal/apth_cancel.h"
#include "internal/apth_sched.h"
#include "internal/apth_thqueue.h"
#include "attr/apth_attr.h"
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
            // Spawn in current scheduler
            sched = CUR_SCHED;
            assert(sched != NULL);
        }
    }

    assert(sched != NULL);

    // Allocate a new thread control block, do all the allocations

    void *stackaddr = iattr->flags & ATTR_FLAG_STACKADDR ? iattr->stackaddr : NULL;
    if ((t = apth_tcb_alloc(iattr->stacksize, stackaddr, iattr->guardsize)) == NULL)
        return apth_error(errno, errno);

    // Standard TCB ingredients
    t->prio = iattr->schedparam.sched_priority;
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
        if (!APTH_IS_FAKE_SCHED(creator))
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
