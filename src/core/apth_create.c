#include "common.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/apth_errno.h"
#include "utils/lll.h"

static void apth_create_trampoline(void)
{
    void *data;
    // If this apth is scheduled to run, then `cur` should point to current apth
    apth_t cur = cur_apth();

    data = (*cur->start_func)(cur->start_arg);

    // Do an implicit exit of the apth with the result value
    // apth_exit(data);
    // Note: we cannot call `apth_exit` here, since that should be called optionally
    // and explicitly by program, marking the process not to exit with the main apth.
    apth_do_cancel(data);
    PANIC("Should not reach here");
}

APTH_INTERNAL int apth_create_internal(
    apth_t *newthr, const apth_attr_t *attr,
    void *(*start_routine)(void *), void *arg, apth_sched_t sched)
{
    apth_t t;
    unsigned int stacksize;
    void *stackaddr;
    apth_time_t ts;
    apth_t cur;
    if (sched == NULL)
    {
        sched = cur_sched();
        cur = sched->cur;
    }
    else
    {
        cur = APTH_NULL;
    }

    // apth_debug("enter");

    // Consistency
    if (start_routine == NULL)
        return apth_error(EINVAL, EINVAL);

    // Support the special case of main()
    // TODO: check what is this

    // Allocate a new thread control block
    stacksize = (attr == NULL ? APTH_STACK_SIZE_DEFAULT : attr->stacksize);
    stackaddr = (attr == NULL ? NULL : attr->stackaddr);

    if ((t = apth_tcb_alloc(stacksize, stackaddr)) == NULL)
        return apth_error(errno, errno);

    // Configure remainning attributes
    // TODO: here check whether the parent fields will be inherited partially
    // even when the attr is set (in pthread)
    if (attr != NULL)
    {
        // TODO
    }
    else if (cur != APTH_NULL)
    {
        // `cur` is the caller thread of `apth_create`, so inherit some fields
        // from the parent thread
    }
    else
    {
        // Defaults
    }

    // Initialize the time points and ranges
    // apth_debug("initializing times");
    apth_time_set(&ts, APTH_TIME_NOW);
    apth_time_set(&t->spawned, &ts);
    apth_time_set(&t->lastran, &ts);
    apth_time_set(&t->running, APTH_TIME_ZERO);

    // Initialize events
    // apth_debug("initializing events");
    list_empty(&t->event_list);

    // Clear raised signals
    // apth_debug("initializing signals");
    sigemptyset(&t->sigpending);
    t->sigpendcnt = 0;

    // Remember the start routine and arguments
    // apth_debug("initializing start routine and arguments");
    t->start_func = start_routine;
    t->start_arg = arg;

    // Initialize join argument
    // apth_debug("initializing join");
    t->join_arg = NULL;
    // t->joinable = false;
    // TODO: here we used a check to check whether attr is NULL.
    // TODO: but we could give `iattr` a default when `attr` is NULL and use iattr anyway.
    t->joinid = attr == NULL ? NULL : (attr->flags & ATTR_FLAG_DETACHSTATE ? t : NULL);

    // Initialize thread specific storage
    // apth_debug("initializing TLS");
    t->specific_used = false;
    t->specific[0] = t->specific_1stblock;

    // Initialize cancellation stuff
    // apth_debug("initializing cancellation stuff");
    t->cancelhandling = 0; // TODO: is this right? we should only clear enable bit
    t->cleanups = NULL;

    t->belongs_to_list = NULL;

    // Initialize sync stuff
    // TODO:

    // TODO: exception stuff

    // Initialize the machine context of this new thread
    // apth_debug("initializing the context of this new thread");
    assert_msg(t->stacksize > 0, "APTH 0x%lx have stack size <= 0", t);

    if (!apth_ctx_set(t->ctx, apth_create_trampoline,
                      t->stack, (char *)((char *)t->stack + t->stacksize)))
    {
        apth_shield
        {
            apth_tcb_free(t);
        }
        return apth_error(EINVAL, EINVAL);
    }

    // Finally insert it into the new queue where
    // the scheduler will pick it up for dispatching
    // apth_debug("sched = %p", sched);
    t->state = APTH_STATE_NEW;

    push_apth_to_new(t, sched); // TODO: is sched initialized by here
    // apth_debug("pushed apth to new");
    // Increment scheduler thread count
    inc_thrcnt(sched);
    inc_alive_thrcnt();

    t->worker = sched->worker;
    *newthr = t;

    // apth_debug("spawned new thread t=%p", t);
    // apth_debug("leave");
    // while(atomic_load_acquire(&SIMPLE_BARRIER) == 0);
    // apth_debug("allow to proceed");
    return 0;
}

int apth_create(apth_t *newthr, const apth_attr_t *attr,
                void *(*start_routine)(void *), void *arg)
{
    // TODO: determine which sched to spawn this apth to
    return apth_create_internal(newthr, attr, start_routine, arg, NULL);
}
