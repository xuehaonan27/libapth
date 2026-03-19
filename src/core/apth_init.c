#include "common.h" // For WORKER_SPAWNED
#include "apth.h"
#include "hook_libc/hooked_funcs.h"
#include "internal/types.h"
#include "internal/apth_worker.h"
#include "internal/apth_signal.h"
#include "internal/apth_fd.h"
#include "internal/apth_preempt.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include "utils/atomic_wrapper.h"

static bool LIBAPTH_INITIALIZED = false;

struct __apth_main_args __LIBAPTH_MAIN_ARGS;

int apth_initvals_init(apth_init_t *initvals, int workers,
                       void *(*main_apth)(void *), void *main_args,
                       apth_attr_t *main_attr)
{
    if (workers <= 0 || main_apth == NULL)
    {
        return -1;
    }
    initvals->workers = workers;
    initvals->main_apth = main_apth;
    initvals->main_args = main_args;
    initvals->main_attr = main_attr;
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
void apth_init(apth_init_t *initvals)
{
    if (LIBAPTH_INITIALIZED)
    {
        errno = EPERM;
        return;
    }
    else
        LIBAPTH_INITIALIZED = true;

    // Initialize syscall wrapping
    if (apth_func_system_init() != 0)
    {
        apth_debug("fail to initialize syscall system");
        return;
    }

    apth_debug("enter");

    // Initialize signal system
    if (apth_signal_system_init() != 0)
    {
        apth_debug("fail to initialize signal system");
        return;
    }

    // Register process level signal catchers
    if (apth_install_kernel_signal_catchers() != 0)
    {
        apth_debug("fail to register process level signal catchers");
        return;
    }

    apth_fd_table_init();

    // Initialize preemption system
    apth_preempt_init();

    // worker_key_t_init();
    sched_key_t_init();

    // TODO: check `initvals`

    // Initialize the scheduler
    if (apth_global_scheduler_pool_init(initvals->workers) != 0)
    {
        apth_shield
        {
            apth_func_system_drop();
            errno = EAGAIN;
            return;
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

    // set_cur_worker(worker0);
    SET_CUR_SCHED(sched);
    SET_CUR_APTH(NULL);

    apth_debug("worker0 = %p", worker0);
    apth_debug("sched = %p", sched);

    apth_attr_t *pmain_attr = NULL;
    apth_attr_t main_attr;

    if (initvals->main_attr != NULL)
        pmain_attr = initvals->main_attr;
    else
    {
        apth_attr_init(&main_attr);
        pmain_attr = &main_attr;
    }
    apth_attr_setname_np(pmain_attr, "main apth");

    apth_t main_th;
    apth_create(&main_th, pmain_attr, initvals->main_apth, initvals->main_args);

    // Setting MAIN_APTH meaning all workers could start to dive into loop.
    // Before that, do more things ...
    set_DEBUG_USING_HOOKED(1);
    set_MAIN_APTH(main_th);

    apth_debug("LIBAPTH INITIALIZER GOING TO CLEAR AND EXIT...");

    // NOTE: debug here, remove this
    // We must hold MAIN PTHREAD here to receive Ctrl-C for debugging...
#ifdef APTH_HOLD_INITIALIZER_PTHREAD
    void *worker0_rslt;
    apth_func_raw(pthread_join)(worker0->tid, &worker0_rslt);
    if (worker0_rslt != NULL)
        apth_debug("worker0_rslt should be NULL, but is %p", worker0_rslt);
    apth_debug("LIBAPTH INITIALIZER JOINED WORKER 0...");
#endif // APTH_HOLD_INITIALIZER_PTHREAD

    SET_CUR_SCHED(NULL);

#ifdef APTH_HOLD_INITIALIZER_PTHREAD
    // Holding initializer thread, then all things cleared when we execute
    // to here. Call LIBC `exit` to exit the whole process.
    apth_func_raw(exit)(0); // TODO: return what the main APTH returns
#else
    // Do not hold initializer thread, then we should silently exit here.
    // Call `pthread_exit`, the initializer pthread should exit but others
    // should continue to run.
    apth_func_raw(pthread_exit)(NULL);
#endif
}

// Drop the libapth package.
void apth_drop(void)
{
    set_DEBUG_USING_HOOKED(0);

    apth_debug("enter");
    if (!LIBAPTH_INITIALIZED)
    {
        errno = EPERM;
        return;
    }

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

    apth_preempt_drop();

    if (apth_func_system_drop() != 0)
    {
        apth_debug("fail to drop LIBAPTH syscall system");
        PANIC("fail to drop LIBAPTH syscall system");
    }

    LIBAPTH_INITIALIZED = false;

    apth_debug("leave");
}