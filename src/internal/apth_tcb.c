#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_errno.h"
#include "utils/apth_sysutils.h"
#include "utils/debug.h"
#include <stdlib.h>
#include <string.h>

APTH_INTERNAL int check_stacksize_attr(size_t st)
{
    if (st >= APTH_STACK_SIZE_DEFAULT)
        return 0;

    return EINVAL;
}

// The most lower 2 bits should be 0, meaning TCB should be at least 4 bytes aligned.
#define IS_VALID_APTH_T(t) (((uintptr_t)(t) & 0x3) == 0)

APTH_INTERNAL apth_t apth_tcb_alloc(size_t stacksize, void *stackaddr)
{
    apth_debug("enter");
    apth_t t;

    if (stacksize > 0 && stacksize < APTH_STACK_SIZE_DEFAULT)
        stacksize = APTH_STACK_SIZE_DEFAULT;
    if ((t = (apth_t)malloc(sizeof(struct apth_st))) == NULL)
        return APTH_NULL;

    assert_msg(IS_VALID_APTH_T(t), "t not aligned to 4 bytes: %p", t);

    memset(t, '\0', sizeof(struct apth_st));

    t->ctx = apth_ctx_alloc();

    t->stacksize = stacksize;
    t->stack_mem_start = NULL;
    t->stackguard = NULL;
    t->stackloan = (stackaddr != NULL ? true : false);
    if (stacksize > 0)
    {
        if (stackaddr != NULL)
        // t->stack_mem_start = (char *)stackaddr;
#if APTH_STACKGROWTH < 0
            t->stack_mem_start = (char *)stackaddr - stacksize;
#else
            t->stack_mem_start = (char *)stackaddr;
#endif
        else
        {
            if ((t->stack_mem_start = (char *)malloc(stacksize)) == NULL)
            {
                apth_shield
                {
                    free(t->ctx);
                    free(t);
                }
                return APTH_NULL;
            }
        }

        // TODO: set guard region, allocating extra space at the end of the stack.
        // TODO: A guard area consists of virtual memory pages that are protected to prevent read and write access.
#if APTH_STACKGROWTH < 0
        /* guard is at lowest address (alignment is guaranteed) */
        t->stackguard = (uint32_t *)(t->stack_mem_start); /* double cast to avoid alignment warning */
#else
        /* guard is at highest address (be careful with alignment) */
        t->stackguard = (uint32_t *)(t->stack_mem_start + (((stacksize / sizeof(long)) - 1) * sizeof(long)));
#endif
        *(uint32_t *)(t->stackguard) = APTH_MAGIC;
    }

    list_init(&t->event_list);
    // TODO: initialize fields

    apth_debug("leave");
    return t;
}

APTH_INTERNAL void apth_tcb_free(apth_t t)
{
    assert(t != NULL);
    apth_sched_t sched = cur_sched();

    if (t->stack_mem_start != NULL && !t->stackloan)
        free(t->stack_mem_start);

    // apth_thread_cleanup(t);

    assert_msg(t->cleanups == NULL, "apth %p try to TCB free without executing cleanups", t);

    // TODO: Clear other fields

    free(t->ctx);
    free(t);

    // Decrement
    dec_thrcnt(sched);

    return;
}
