#define _GNU_SOURCE
#include <sched.h>

#include "common.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/apth_errno.h"
#include "utils/apth_sysutils.h"
#include "utils/lll.h"

static void apth_create_trampoline(void)
{
    void *data;
    // If this apth is scheduled to run, then `cur` should point to current apth
    apth_t cur = cur_apth();

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
    apth_attr_t iattr = NULL;
    apth_time_t ts;
    apth_t cur;
    apth_sched_t sched = NULL;
    apth_worker_t worker = NULL;

    // Consistency
    if (start_routine == NULL)
        return apth_error(EINVAL, EINVAL);

    if (attr != NULL)
    {
        iattr = *attr;
        if (iattr == NULL)
            return apth_error(EINVAL, EINVAL);
    }
    else
        apth_attr_init(&iattr);
    assert(iattr != NULL);

    if (newthr == get_addr_of_MAIN_APTH())
    {
        // We are spawning main thread. `apth_init` should have prepared proper
        // worker TLS for us.
        worker = cur_worker();
        sched = cur_sched();
        assert(sched != NULL);
        assert(sched == worker->sched);
        assert(worker == sched->worker);
        cur = sched->cur;
        assert(APTH_IS_FAKE_SCHED(cur));
    }
    else
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
            cur = sched->cur;
        }
        else
        {
            // Spawn in current scheduler
            worker = cur_worker();
            sched = cur_sched();
            cur = sched->cur;
        }
    }

    assert(sched != NULL);

    // Allocate a new thread control block, do all the allocations

    void *stackaddr = iattr->flags & ATTR_FLAG_STACKADDR ? iattr->stackaddr : NULL;
    if ((t = apth_tcb_alloc(iattr->stacksize, stackaddr)) == NULL)
        return apth_error(errno, errno);

    // Standard TCB ingredients
    t->prio = iattr->schedparam.sched_priority;
    memcpy(t->name, iattr->name, APTH_TCB_NAMELEN);
    t->name[APTH_TCB_NAMELEN] = '\0';
    t->dispatches = 0;
    // t->state = APTH_STATE_NEW;
    submit_desired_state_to(t, APTH_STATE_NEW, "apth_create");

    // Timing: initialize the time points and ranges
    apth_time_set(&ts, APTH_TIME_NOW);
    apth_time_set(&t->spawned, &ts);
    apth_time_set(&t->lastran, &ts);
    apth_time_set(&t->running, APTH_TIME_ZERO);

    // Events
    list_empty(&t->event_list);

    // Signals
    sigemptyset(&t->sigpending);
    t->sigpendcnt = 0;

    // Remember the start routine and arguments
    t->start_func = start_routine;
    t->start_arg = arg;

    // Initialize join argument
    t->join_arg = NULL;
    t->joinid = iattr->flags & ATTR_FLAG_DETACHSTATE ? t : NULL;

    // Initialize cancellation stuff
    t->cancelreq = false;
    t->cancelhandling = 0; // TODO: is this right? we should only clear enable bit
    t->cleanups = NULL;

    // TODO: Initialize sync stuff

    // TODO: exception stuff

    // Initialize thread specific storage
    t->specific[0] = t->specific_1stblock;
    t->specific_used = false;

    // Scheduler list handling
    /*set_sched_of(t, sched);
    set_belonging_list_of(t, NULL);
    set_belonging_list_lock_of(t, NULL);*/

    // Initialize the machine context of this new thread
    assert_msg(t->stacksize > 0, "APTH 0x%lx have stack size <= 0", t);

    if (!apth_ctx_set(t->ctx, apth_create_trampoline, t->stack_mem_start, t->stacksize))
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
    push_apth_to(sched->new_queue, t);

    // Increment scheduler thread count
    inc_thrcnt(sched);
    inc_alive_thrcnt();

    apth_debug("Now alive thread count = %u", get_apth_alive_nthreads());

    *newthr = t;
    return 0;
}
