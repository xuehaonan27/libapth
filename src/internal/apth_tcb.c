#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_errno.h"

apth_t apth_tcb_alloc(size_t stacksize, void *stackaddr)
{
    apth_t t;

    if (stacksize > 0 && stacksize < APTH_STACK_SIZE_DEFAULT)
        stacksize = APTH_STACK_SIZE_DEFAULT;
    if ((t = (apth_t)malloc(sizeof(struct apth_st))) == NULL)
        return (apth_t)NULL;

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
                return (apth_t)NULL;
            }
        }

#if APTH_STACKGROWTH < 0
        /* guard is at lowest address (alignment is guarrantied) */
        t->stackguard = (long *)((long)t->stack); /* double cast to avoid alignment warning */
#else
        /* guard is at highest address (be careful with alignment) */
        t->stackguard = (long *)(t->stack + (((stacksize / sizeof(long)) - 1) * sizeof(long)));
#endif
        *(uint32_t *)(t->stackguard) = 0xCAFEBABE;
    }

    return t;
}

void apth_tch_free(apth_t t)
{
    if (t == NULL)
        return;
    if (t->stack != NULL && !t->stackloan)
        free(t->stack);
    if (t->data_value != NULL)
        free(t->data_value);
    if (t->cleanups != NULL)
        apth_cleanup_popall(t, false);
    free(t);
    return;
}