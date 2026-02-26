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
    if (apth_syscall_system_init() != 0)
    {
        apth_debug("fail to initialize syscall system");
        return apth_error(-1, errno);
    }

    apth_debug("enter");

    // Initialize signal system
    if (apth_signal_system_init() != 0)
    {
        apth_debug("fail to initialize signal system");
        return apth_error(-1, errno);
    }

    // Register process level signal catchers
    if (apth_install_kernel_signal_catchers() != 0)
    {
        apth_debug("fail to register process level signal catchers");
        return apth_error(-1, errno);
    }

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
    // TODO: more reliable sync
    while (atomic_load_acquire(&WORKER_SPAWNED) != 0)
    {
        sched_yield();
    }

    apth_debug("All spawned");

    // Spawn the main thread

    // NOTE: `get_worker_by_id` requires lll, which means TLS should be initialized.
    // Getting worker 0 should go through fast path, without acquiring the lll.
    apth_worker_t worker0 = get_worker_by_id(0);
    apth_sched_t sched = worker0->sched;

    set_cur_worker(worker0);
    set_cur_sched(sched);
    set_cur_apth(APTH_FAKE_SCHED(sched));

    apth_debug("worker0 = %p", worker0);
    apth_debug("sched = %p", sched);

    apth_attr_t main_attr;
    apth_attr_init(&main_attr);
    apth_attr_setname_np(&main_attr, "main apth");

    apth_t main_th;
    apth_create(&main_th, &main_attr, initvals->main_apth, initvals->main_args);
    apth_attr_destroy(&main_attr);

    // Setting MAIN_APTH meaning all workers could start to dive into loop.
    // Before that, do more things ...
    set_DEBUG_USING_HOOKED(1);
    set_MAIN_APTH(main_th);

    apth_debug("LIBAPTH INITIALIZER GOING TO CLEAR AND EXIT...");

    // NOTE: debug here, remove this
    // We must hold MAIN PTHREAD here to receive Ctrl-C for debugging...
#if defined(APTH_DEBUG) && defined(APTH_DEBUG_HOLD_INITIALIZER_PTHREAD)
    void *worker0_rslt;
    apth_syscall_raw(pthread_join)(worker0->tid, &worker0_rslt);
    if (worker0_rslt != NULL)
        apth_debug("worker0_rslt should be NULL, but is %p", worker0_rslt);
    apth_debug("LIBAPTH INITIALIZER JOINED WORKER 0...");
#endif // APTH_DEBUG_HOLD_INITIALIZER_PTHREAD

    set_cur_sched(NULL);
    set_cur_worker(NULL);

    // Call `pthread_exit`, the initializer pthread should exit but others
    // should continue to run
    apth_syscall_raw(pthread_exit)(NULL);
    return 0;
}

// Drop the libapth package.
int apth_drop(void)
{
    set_DEBUG_USING_HOOKED(0);

    apth_debug("enter");
    if (!LIBAPTH_INITIALIZED)
        return apth_error(EINVAL, EINVAL);

    if (apth_global_scheduler_pool_drop() != 0)
    {
        apth_debug("fail to drop global scheduler pool");
        PANIC("fail to drop global scheduler pool");
    }

    if (apth_signal_system_drop() != 0)
    {
        apth_debug("fail to drop global scheduler pool");
        PANIC("fail to drop global signal system");
    }

    if (apth_syscall_system_drop() != 0)
    {
        apth_debug("fail to drop LIBAPTH syscall system");
        PANIC("fail to drop LIBAPTH syscall system");
    }

    LIBAPTH_INITIALIZED = false;

    apth_debug("leave");
    return 0;
}