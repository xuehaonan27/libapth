#include "internal_funcs.h"
#include "utils/debug.h"
#include "utils/archplattoold.h"
#include "utils/apth_errno.h"
#include <stdlib.h>

bool apth_cleanup_push(void (*func)(void *), void *arg)
{
    apth_cleanup_t cleanup;
    if (func == NULL)
        return apth_error(false, EINVAL);
    if ((cleanup = (apth_cleanup_t)malloc(sizeof(struct apth_cleanup_st))) == NULL)
        return apth_error(false, ENOMEM);
    cleanup->func = func;
    cleanup->arg = arg;
    cleanup->next = cur_apth()->cleanups;
    cur_apth()->cleanups = cleanup;
    return true;
}

bool apth_cleanup_pop(int execute)
{
    apth_cleanup_t cleanup;
    int retval = false;

    if ((cleanup = cur_apth()->cleanups) != NULL)
    {
        cur_apth()->cleanups = cleanup->next;
        if (execute)
            cleanup->func(cleanup->arg);
        free(cleanup);
        retval = true;
    }
    return retval;
}
