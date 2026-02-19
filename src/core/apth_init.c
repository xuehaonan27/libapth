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

/*
 * apth_config_defaults - apply built-in default values to *cfg.
 *
 * This is the single source of truth for default init parameters.
 * When new fields are added to apth_init_t, add their defaults here so
 * that both the weak apth_configure and any user APTH_CONFIG override
 * automatically inherit sensible values for fields they don't mention.
 *
 * NOTE: main_apth / main_args are intentionally left at NULL here;
 *       they are wired up by APTH_MAIN_BEGIN, never by the user.
 */
void apth_config_defaults(apth_init_t *cfg)
{
    cfg->workers = 1;
    cfg->main_apth = NULL;
    cfg->main_args = NULL;
}

/*
 * apth_configure - weak default library configuration hook.
 *
 * Users can override this symbol (or use the APTH_CONFIG macro) to
 * customise initialisation parameters without touching main_apth/main_args.
 * The linker will prefer any strong definition provided by user code.
 */
__attribute__((weak)) void apth_configure(apth_init_t *cfg)
{
    apth_config_defaults(cfg);
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

    fprintf(stderr, "apth_init: enter\n");

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

    fprintf(stderr, "All spawned\n");

    // Spawn the main thread

    // NOTE: `get_worker_by_id` requires lll, which means TLS should be initialized.
    // Getting worker 0 should go through fast path, without acquiring the lll.
    apth_worker_t worker0 = get_worker_by_id(0);
    apth_sched_t sched = worker0->sched;

    set_cur_worker(worker0);
    set_cur_sched(sched);
    set_cur_apth(APTH_FAKE_SCHED(sched));

    // apth_debug("worker0 = %p", worker0);
    fprintf(stderr, "worker0 = %p\n", worker0);
    fprintf(stderr, "sched = %p\n", sched);

    // apth_debug("spawning main thread at scheduler: %p", sched);
    apth_t main_th;
    apth_create_internal(&main_th, NULL, initvals->main_apth, initvals->main_args, sched);
    apth_setname_np(main_th, "main apth");
    atomic_store_release(&MAIN_APTH, main_th);
    // apth_debug("spawned main thread: %p (\"%s\")", main_th, main_th->name);
    // apth_debug("apth_init: leave");

    // Here we determine that libapth is initialized
    // atomic_store_release(&SYNC_BEFORE_MAIN_APTH_SPAWN, 1);

    // NOTE: debug here, remove this
    // We must hold MAIN PTHREAD here to receive Ctrl-C for debugging...
#if defined(APTH_DEBUG) && defined(APTH_DEBUG_HOLD_INITIALIZER_PTHREAD)
    for (;;)
        ;
#endif // APTH_DEBUG_HOLD_INITIALIZER_PTHREAD

    set_cur_sched(NULL);
    set_cur_worker(NULL);

    // Call `pthread_exit`, the initializer pthread should exit but others
    // should continue to run
    fprintf(stderr, "LIBAPTH INITIALIZER PTHREAD EXITING...\n");
    apth_syscall_raw(pthread_exit)(NULL);
    return 0;
}

// Drop the libapth package.
int apth_drop(void)
{
    fprintf(stderr, "apth_drop: enter\n");
    if (!LIBAPTH_INITIALIZED)
        return apth_error(EINVAL, EINVAL);
    // apth_debug("apth_drop: enter");

    if (apth_global_scheduler_pool_drop() != 0)
    {
        // apth_debug("apth_drop: fail to drop global scheduler pool");
        fprintf(stderr, "apth_drop: fail to drop global scheduler pool");
    }

    if (apth_syscall_system_drop() != 0)
    {
        // apth_debug("apth_drop: fail to drop LIBAPTH syscall system");
        fprintf(stderr, "apth_drop: fail to drop LIBAPTH syscall system");
    }

    LIBAPTH_INITIALIZED = false;
    // apth_debug("apth_drop: leave");
    return 0;
}