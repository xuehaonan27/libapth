#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_errno.h"

bool apth_cleanup_push(void (*func)(void *), void *arg)
{
    apth_cleanup_t cleanup;
    if (func == NULL)
        return apth_error(false, EINVAL);
    if ((cleanup = (apth_cleanup_t)malloc(sizeof(struct apth_cleanup_st))) == NULL)
        return apth_error(false, ENOMEM);
    cleanup->func = func;
    cleanup->arg = arg;
    cleanup->next = apth_current()->cleanups;
    apth_current()->cleanups = cleanup;
    return true;
}

bool apth_cleanup_pop(bool execute)
{
    apth_cleanup_t cleanup;
    int retval = false;

    if ((cleanup = apth_current()->cleanups) != NULL)
    {
        apth_current()->cleanups = cleanup->next;
        if (execute)
            cleanup->func(cleanup->arg);
        free(cleanup);
        retval = true;
    }
    return retval;
}

void apth_cleanup_popall(apth_t t, bool execute)
{
    apth_cleanup_t cleanup;

    while ((cleanup = t->cleanups) != NULL)
    {
        t->cleanups = cleanup->next;
        if (execute)
            cleanup->func(cleanup->arg);
        free(cleanup);
    }
    return;
}