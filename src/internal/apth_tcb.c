#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_errno.h"
#include "utils/apth_sysutils.h"
#include "utils/debug.h"
#include <stdlib.h>
#include <string.h>

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
    t->stack = NULL;
    t->stackguard = NULL;
    t->stackloan = (stackaddr != NULL ? true : false);
    if (stacksize > 0)
    {
        if (stackaddr != NULL)
            t->stack = (char *)stackaddr;
        else
        {
            if ((t->stack = (char *)malloc(stacksize)) == NULL)
            {
                apth_shield { free(t); }
                return APTH_NULL;
            }
        }

#if APTH_STACKGROWTH < 0
        /* guard is at lowest address (alignment is guaranteed) */
        t->stackguard = (uint32_t *)(t->stack); /* double cast to avoid alignment warning */
#else
        /* guard is at highest address (be careful with alignment) */
        t->stackguard = (uint32_t *)(t->stack + (((stacksize / sizeof(long)) - 1) * sizeof(long)));
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
    if (t == NULL)
        return;
    if (t->stack != NULL && !t->stackloan)
        free(t->stack);

    apth_thread_clenaup(t);

    // Clear other fields
    // TODO("Clear other fields");
    free(t->ctx);
    free(t);

    // Decrement
    apth_sched_t sched = cur_sched();
    dec_thrcnt(sched);

    return;
}