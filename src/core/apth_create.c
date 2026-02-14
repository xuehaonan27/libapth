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
    apth_exit(data);
    PANIC("Should not reach here");
}

int apth_create(apth_t *newthr, const apth_attr_t *attr,
                void *(*start_routine)(void *), void *__arg)
{
    apth_t t;
    unsigned int stacksize;
    void *stackaddr;
    apth_time_t ts;
    apth_t cur = cur_apth();

    apth_debug("apth_create: enter");

    // Consistency
    if (start_routine == NULL)
        return apth_error(APTH_NULL, EINVAL);

    // Support the special case of main()
    // TODO: check what is this

    // Allocate a new thread control block
    stacksize = (attr == NULL ? APTH_STACK_SIZE_DEFAULT : attr->stacksize);
    stackaddr = (attr == NULL ? NULL : attr->stackaddr);

    if ((t = apth_tcb_alloc(stacksize, stackaddr)) == NULL)
        return apth_error(APTH_NULL, errno);

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
    apth_time_set(&ts, APTH_TIME_NOW);
    apth_time_set(&t->spawned, &ts);
    apth_time_set(&t->lastran, &ts);
    apth_time_set(&t->running, APTH_TIME_ZERO);

    // Initialize events
    list_empty(&t->event_list);

    // Clear raised signals
    sigemptyset(&t->sigpending);
    t->sigpendcnt = 0;

    // Remember the start routine and arguments
    t->start_func = start_routine;
    t->start_arg = __arg;

    // Initialize join argument
    t->join_arg = NULL;
    // t->joinable = false;
    t->joinid = attr->flags & ATTR_FLAG_DETACHSTATE ? t : NULL;

    // Initialize thread specific storage
    t->specific_used = false;
    memset(t->specific, 0, sizeof(t->specific));
    memset(t->specific_1stblock, 0, sizeof(t->specific_1stblock));

    // Initialize cancellation stuff
    t->cancelhandling = 0; // TODO: is this right? we should only clear enable bit
    t->cleanups = NULL;

    t->belongs_to_list = NULL;

    // Initialize sync stuff
    // TODO:

    // TODO: exception stuff

    // Initialize the machine context of this new thread
    assert_msg(t->stacksize > 0, "APTH 0x%lx have stack size <= 0", t);
    if (!apth_ctx_set(&t->ctx, apth_create_trampoline,
                      t->stack, (char *)(t->stack + t->stacksize)))
    {
        apth_shield
        {
            apth_tcb_free(t);
        }
        return apth_error(APTH_NULL, errno);
    }

    // Finally insert it into the new queue where
    // the scheduler will pick it up for dispatching
    t->state = APTH_STATE_NEW;
    push_apth_to_new(t, cur_sched()); // TODO: is sched initialized by here

    apth_debug("apth_create: leave");
    return t;
}
