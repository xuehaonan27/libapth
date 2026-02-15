#include "apth.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include "utils/atomic_wrapper.h"

static bool LIBAPTH_INITIALIZED = false;

int apth_initvals_init(apth_init_t *initvals, int workers,
                       void *(*main_apth)(void *), void *main_args)
{
    if (workers <= 0 || main_apth == NULL)
    {
        return -1;
    }
    initvals->workers = workers;
    initvals->main_apth = main_apth;
    initvals->main_args = main_args;
    return 0;
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

    // TODO: add synchronization here, making sure all workers are running now
    // TOOD: Maybe checking whether all scheds are opening is useful
    // TODO: more reliable sync
    while (atomic_load_acquire(&WORKER_SPAWNED) != 0)
    {
        sched_yield();
    }

    // Spawn the main thread
    apth_worker_t worker0 = get_worker_by_id(0);
    apth_debug("worker0 = %p", worker0);
    apth_sched_t sched = worker0->sched;

    apth_debug("spawning main thread at scheduler: %p", sched);
    apth_t main_th;
    apth_create_internal(&main_th, NULL, initvals->main_apth, initvals->main_args, sched);

    apth_debug("spawned main thread: %p (\"%s\")", main_th, main_th->name);

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