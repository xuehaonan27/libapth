#include "apth.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"

static bool LIBAPTH_INITIALIZED = false;

int apth_initvals_init(apth_init_t *initvals, int workers)
{
    if (workers <= 0)
    {
        return -1;
    }
    initvals->workers = workers;
}

// Initialize the libapth package.
int apth_init(apth_init_t *initvals)
{
    if (LIBAPTH_INITIALIZED)
        return apth_error(EPERM, EPERM);
    else
        LIBAPTH_INITIALIZED = true;

    // Initialize syscall wrapping
    apth_syscall_system_init();

    apth_debug("apth_init: enter");

    worker_key_t_init();
    sched_key_t_init();

    // TODO: check `initvals`

    // Initialize the scheduler
    if (apth_global_scheduler_pool_init(initvals->workers) != 0)
    {
        apth_shield
        {
            apth_syscall_system_drop();
            return apth_error(EAGAIN, EAGAIN);
        }
    }

    // TODO: optional support for exceptional handling

    apth_debug("apth_init: leave");
    return 0;
}

// Drop the libapth package.
int apth_drop(void)
{
    if (!LIBAPTH_INITIALIZED)
        return apth_error(EINVAL, EINVAL);
    apth_debug("apth_drop: enter");

    if (apth_global_scheduler_pool_drop() != 0)
    {
        apth_debug("apth_drop: fail to drop global scheduler pool");
    }

    if (apth_syscall_system_drop() != 0)
    {
        apth_debug("apth_drop: fail to drop LIBAPTH syscall system");
    }

    LIBAPTH_INITIALIZED = false;
    apth_debug("apth_drop: leave");
    return 0;
}