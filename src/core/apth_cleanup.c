#include "internal_funcs.h"
#include "utils/debug.h"
#include "utils/archplattoold.h"
#include "utils/apth_errno.h"
#include <stdlib.h>

// TODO: POSIX `pthread_cleanup_push` `pthread_cleanup_pop` would not
// produce errors though.

void apth_cleanup_push(void (*func)(void *), void *arg)
{
    apth_t cur = cur_apth();

    apth_cleanup_t cleanup;
    if (func == NULL)
        // return apth_error(void, EINVAL);
        errno = EINVAL;
    if ((cleanup = (apth_cleanup_t)malloc(sizeof(struct apth_cleanup_st))) == NULL)
    {
        errno = ENOMEM;
        return;
    }
    cleanup->func = func;
    cleanup->arg = arg;
    cleanup->next = cur->cleanups;
    cur->cleanups = cleanup;
}

void apth_cleanup_pop(int execute)
{
    apth_cleanup_t cleanup;
    apth_t cur = cur_apth();

    if ((cleanup = cur->cleanups) != NULL)
    {
        cur->cleanups = cleanup->next;
        if (execute != 0)
            cleanup->func(cleanup->arg);
        free(cleanup);
    }
}
