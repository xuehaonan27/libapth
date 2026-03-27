#include "common.h" // For WORKER_SPAWNED
#include "apth.h"
#include "hook_libc/hooked_funcs.h"
#include "internal/types.h"
#include "internal/apth_worker.h"
#include "internal/apth_signal.h"
#include "internal/apth_fd.h"
#include "internal/apth_reactor.h"
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

/*
 * apth_init_common - shared initialization for both apth_init() and
 * apth_init_library().  Initializes all subsystems, creates scheduler
 * workers, and waits for them to be ready.
 *
 * Returns 0 on success, -1 on error.
 */
static int apth_init_common(int workers)
{
    if (LIBAPTH_INITIALIZED)
    {
        errno = EPERM;
        return -1;
    }
    LIBAPTH_INITIALIZED = true;

    // Initialize syscall wrapping
    if (apth_func_system_init() != 0)
    {
        apth_debug("fail to initialize syscall system");
        return -1;
    }

    apth_debug("enter");

    // Initialize signal system
    if (apth_signal_system_init() != 0)
    {
        apth_debug("fail to initialize signal system");
        return -1;
    }

    // Register process level signal catchers
    if (apth_install_kernel_signal_catchers() != 0)
    {
        apth_debug("fail to register process level signal catchers");
        return -1;
    }

    apth_fd_table_init();

    // Start the global I/O reactor (dedicated pthread for epoll_wait).
    // Must happen before scheduler pool init so schedulers can submit
    // FD watch requests to the reactor from the start.
    if (apth_reactor_start() != 0)
    {
        apth_debug("WARNING: failed to start reactor, falling back to per-scheduler epoll");
        // Not fatal — per-scheduler epoll is the fallback
    }

    // Initialize preemption system
    apth_preempt_init();

    // worker_key_t_init();
    sched_key_t_init();

    // TODO: check `initvals`

    // Initialize the scheduler
    if (apth_global_scheduler_pool_init(workers) != 0)
    {
        apth_shield
        {
            apth_func_system_drop();
            errno = EAGAIN;
            return -1;
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
    return 0;
}

// Initialize the libapth package.
void apth_init(apth_init_t *initvals)
{
    if (apth_init_common(initvals->workers) != 0)
        return;

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

// In library mode, the user calls apth_drop() explicitly instead of
// having worker0 trigger it, so worker0 must skip its own apth_drop().
bool __apth_library_mode = false;

// Static sentinel object used as MAIN_APTH in library mode.
// No actual thread is created — this is just enough to satisfy the
// workers' spin-wait for MAIN_APTH != NULL and the end-of-process check.
static struct apth_st __library_sentinel;

// Dummy scheduler for the host pthread in library mode.
static struct apth_sched_st __library_host_sched;

/*
 * apth_init_library - library-mode initialization.
 *
 * Starts LIBAPTH schedulers in the background without taking over the
 * calling pthread.  The caller can then create apth threads via
 * apth_create() and later shut down with apth_drop().
 *
 * Design: Instead of creating a real sentinel thread (which creates race
 * conditions during shutdown), we use a static sentinel OBJECT that
 * satisfies the workers' MAIN_APTH check.  Workers in library mode never
 * self-terminate — they only exit when apth_worker_drop() sets opening=false.
 *
 * Returns 0 on success, -1 on error.
 */
int apth_init_library(int workers)
{
    if (apth_init_common(workers) != 0)
        return -1;

    // Dummy scheduler for the calling pthread (so CUR_APTH works)
    memset(&__library_host_sched, 0, sizeof(__library_host_sched));
    __library_host_sched.id = -1;
    __library_host_sched.wake_eventfd = -1;
    __library_host_sched.epoll_fd = -1;
    __library_host_sched.cur = NULL;
    SET_CUR_SCHED(&__library_host_sched);

    __apth_library_mode = true;

    // Set up the static sentinel as MAIN_APTH.
    // Pretend the "main apth" already exited via apth_exit — this keeps
    // workers alive (worker0_check_end_process checks alive_thread_count
    // when MAIN_APTH_EXITED_BY_CALLING_APTH_EXIT is set, but in library
    // mode that check is skipped entirely).
    memset(&__library_sentinel, 0, sizeof(__library_sentinel));
    __library_sentinel.magic = APTH_MAGIC;
    atomic_store_release(&__library_sentinel.state, APTH_STATE_TERMINATED);

    set_DEBUG_USING_HOOKED(1);
    set_MAIN_APTH(&__library_sentinel);

    // Mark that the "main apth" has already exited.
    // In library mode, worker0_check_end_process is skipped, so these
    // flags don't trigger self-termination — workers only exit when
    // apth_worker_drop() sets opening=false during apth_drop().
    atomic_store_release(&MAIN_APTH_EXITED, 1);
    atomic_store_release(&MAIN_APTH_EXITED_BY_CALLING_APTH_EXIT, 1);

    return 0;
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

    // In library mode, just clear the flag. Workers will be stopped by
    // apth_global_scheduler_pool_drop() setting opening=false.
    if (__apth_library_mode)
        __apth_library_mode = false;

    // Stop the reactor FIRST — it holds references to scheduler pointers
    // (via waiter->th->current_sched) and wakes schedulers via eventfd.
    // Must stop before schedulers are freed.
    apth_reactor_stop();

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

    apth_fd_table_destroy();

    if (apth_func_system_drop() != 0)
    {
        apth_debug("fail to drop LIBAPTH syscall system");
        PANIC("fail to drop LIBAPTH syscall system");
    }

    LIBAPTH_INITIALIZED = false;

    apth_debug("leave");
}