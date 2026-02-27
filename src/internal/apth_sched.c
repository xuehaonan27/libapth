#include "common.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/apth_errno.h"
#include "utils/lll.h"
#include <stdlib.h>

#include <sys/epoll.h>

// Total APTH threads we have. Note this counter is shared across the process,
// So it should be _Atomic.
_Atomic(unsigned int) apth_nthreads = 0;
_Atomic(unsigned int) apth_alive_nthreads = 0;

static _Atomic(apth_t) MAIN_APTH = APTH_NULL;
_Atomic(int) MAIN_APTH_EXITED = 0;
_Atomic(int) MAIN_APTH_EXITED_BY_CALLING_APTH_EXIT = 0;

APTH_INTERNAL apth_t get_MAIN_APTH(void)
{
    return atomic_load_acquire(&MAIN_APTH);
}

APTH_INTERNAL apth_t *get_addr_of_MAIN_APTH(void)
{
    return (apth_t *)(&MAIN_APTH);
}

APTH_INTERNAL void set_MAIN_APTH(apth_t main_th)
{
    atomic_store_release(&MAIN_APTH, main_th);
}

// Initialize a scheduler onto `worker` and put the new scheduler in `sched`.
APTH_INTERNAL bool apth_scheduler_init(apth_sched_t sched, apth_worker_t worker)
{
    apth_debug("enter");
    sched->id = worker->worker_id;

    // Initialize scheduler context
    sched->sched_ctx = apth_ctx_alloc();
    if (sched->sched_ctx == NULL)
        return apth_error(false, ENOMEM);

    thqueue_init(&sched->new_queue, sched, APTH_STATE_NEW);
    thqueue_init(&sched->ready_queue, sched, APTH_STATE_READY);
    thqueue_init(&sched->waiting_queue, sched, APTH_STATE_WAITING);
    thqueue_init(&sched->terminated_queue, sched, APTH_STATE_TERMINATED);
    thqueue_init(&sched->waked_queue, sched, APTH_STATE_WAKED);
    thqueue_init(&sched->running_queue, sched, APTH_STATE_RUNNING);

    sched->worker = worker;
    sched->switches = 0;
    sched->thrcnt = 0;
    apth_time_set(&sched->running, APTH_TIME_ZERO);
    sched->cur = APTH_NULL;

    // Initialize load support
    apth_time_set(&sched->apth_loadticknext, APTH_TIME_NOW);
    sched->loadval = 1.0;
    sched->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (sched->epoll_fd < 0)
        return apth_error(false, errno);
    // sched->epoll_fd_count = 0;
    // Initialize fd slot table
    for (int i = 0; i < APTH_EPOLL_FD_SLOT_TABLE_SIZE; i++)
    {
        sched->fd_slot_table[i].fd = i;
        sched->fd_slot_table[i].aggregate_events = 0;
        list_init(&sched->fd_slot_table[i].waiters);
        sched->fd_slot_table[i].waiter_count = 0;
        sched->fd_slot_table[i].registered = false;
    }
    list_init(&sched->active_fd_slots);
    sched->active_fd_count = 0;

    // Store the sched into the worker
    worker->sched = sched;

    // Mark the scheduler as opening
    atomic_store_release(&sched->opening, true);
    apth_debug("leave");
    return true;
}

APTH_INTERNAL void inc_thrcnt(apth_sched_t sched)
{
    atomic_fetch_add_release(&sched->thrcnt, 1);
    atomic_fetch_add_release(&apth_nthreads, 1);
}

APTH_INTERNAL void dec_thrcnt(apth_sched_t sched)
{
    atomic_decrement_if_positive(&sched->thrcnt);
    atomic_decrement_if_positive(&apth_nthreads);
}

APTH_INTERNAL unsigned int get_apth_nthreads(void)
{
    return atomic_load_acquire(&apth_nthreads);
}

APTH_INTERNAL void inc_alive_thrcnt(void)
{
    atomic_fetch_add_release(&apth_alive_nthreads, 1);
}

APTH_INTERNAL void dec_alive_thrcnt(void)
{
    atomic_decrement_if_positive(&apth_alive_nthreads);
}

APTH_INTERNAL unsigned int get_apth_alive_nthreads(void)
{
    return atomic_load_acquire(&apth_alive_nthreads);
}

APTH_INTERNAL bool apth_sched_is_opening(apth_sched_t sched)
{
    return atomic_load_acquire(&sched->opening);
}

static apth_time_t apth_loadtickgap = APTH_TIME(1, 0);

APTH_INTERNAL void apth_sched_calc_load(apth_sched_t sched, apth_time_t *now)
{
    if (apth_time_cmp(now, &sched->apth_loadticknext) >= 0)
    {
        apth_time_t ttmp;
        int numready = thqueue_size(sched->ready_queue);
        apth_time_set(&ttmp, now);
        do
        {
            sched->loadval = (numready * 0.25) + (sched->loadval * 0.75);
            apth_time_sub(&ttmp, &apth_loadtickgap);
        } while (apth_time_cmp(&ttmp, &sched->apth_loadticknext) >= 0);
        apth_time_set(&sched->apth_loadticknext, now);
        apth_time_add(&sched->apth_loadticknext, &apth_loadtickgap);
    }
}

static void __drain_free_th(apth_t t)
{
    if (t->join_arg == NULL)
    {
        t->join_arg = APTH_CANCELED;
    }
    apth_thread_cleanup(t);
    apth_tcb_free(t);
}

// Kill the schduler ingredients
APTH_INTERNAL void apth_scheduler_kill(void)
{
    apth_sched_t sched = cur_sched();

    drain_thqueue(sched->new_queue, __drain_free_th);
    drain_thqueue(sched->ready_queue, __drain_free_th);
    drain_thqueue(sched->waiting_queue, __drain_free_th);
    drain_thqueue(sched->terminated_queue, __drain_free_th);
    drain_thqueue(sched->waked_queue, __drain_free_th);
    drain_thqueue(sched->running_queue, __drain_free_th);

    free(sched->new_queue);
    free(sched->ready_queue);
    free(sched->waiting_queue);
    free(sched->terminated_queue);
    free(sched->waked_queue);
    free(sched->running_queue);

    // TODO: report scheduler statistics if in debugging mode

    // Drop epoll
    if (sched->epoll_fd >= 0)
    {
        // Use raw close to avoid recursion
        apth_syscall_raw(close)(sched->epoll_fd);
        sched->epoll_fd = -1;
    }

    // Waiters in active_fd_slots and fd_slot_table should have been cleared
    // in drain_thqueue process and memory freed. No more clean to do.

    // Signal mask restore, allow all signals
    sigset_t sigs;
    sigemptyset(&sigs);
    apth_syscall_raw(pthread_sigmask)(SIG_SETMASK, &sigs, NULL);

    // Cancel TLS, no need for set current apth to APTH_NULL
    // since clearing scheduler will do this.
    set_cur_sched(NULL);
    set_cur_worker(NULL);

    sched_key_t_drop();
    worker_key_t_drop();

    // Free sched
    free((void *)sched->sched_ctx);
    free((void *)sched);
    return;
}

static inline bool worker0_check_end_process(apth_worker_t worker)
{
    // Only main worker could end the process
    if (!is_main_worker(worker))
        return false;

    // Process should be alive if main apth is still alive
    if (atomic_load_acquire(&MAIN_APTH_EXITED) == 0)
        return false;

    bool end_the_process = false;
    if (atomic_load_acquire(&MAIN_APTH_EXITED_BY_CALLING_APTH_EXIT) != 0)
    {
        // Main apth exited by calling `apth_exit`, that means
        // we should not end the process just because main apth exited.
        if (get_apth_alive_nthreads() == 0)
            // Rather, although the main exited by calling `apth_exit`,
            // all other apth exited as well. End the process then.
            end_the_process = true;
        // else: Other threads are still running, we cannot end.
    }
    else
        // Main apth exited without calling `apth_exit`, that means
        // we should end the process here.
        end_the_process = true;
    return end_the_process;
}

// Start routine for a scheduler pthread. The prototype of this function matches
// that required by Pthread.
APTH_INTERNAL void *scheduler_routine(void *arg)
{
    // Now we are in a separated Pthread
    apth_worker_arg_t worker_arg = (apth_worker_arg_t)arg;
    apth_worker_t me = worker_arg->self;

    apth_debug("WORKER %d entered routine", me->worker_id);

    // Allocate and initialize the scheduler
    apth_sched_t sched;
    if ((sched = (apth_sched_t)malloc(sizeof(struct apth_perpthr_scheduler))) == NULL)
        return apth_error(NULL, ENOMEM);
    if (!apth_scheduler_init(sched, me))
    {
        // Fail to initialize
        return apth_error(NULL, errno);
    }

    apth_debug("WORKER %d created scheduler", me->worker_id);

    // Set TLS
    set_cur_worker(me);
    set_cur_sched(sched);
    set_cur_apth(APTH_FAKE_SCHED(sched));

    sigset_t sigs;
    apth_time_t snapshot;
    apth_time_t running;
    struct sigaction sa;
    sigset_t ss;

    // block all signals in the scheduler thread
    sigfillset(&sigs);
    apth_syscall_raw(pthread_sigmask)(SIG_SETMASK, &sigs, NULL);

    // initialize the snapshot time for bootstrapping the loop
    apth_time_set(&snapshot, APTH_TIME_NOW);

    // TODO: initialize other parts of the worker

    // Here we atomically substract WORKER_SPAWNED by 1
    atomic_fetch_sub(&WORKER_SPAWNED, 1);

    // Wait for the main apth to be spawned, before we can continue
    while (get_MAIN_APTH() == APTH_NULL)
        ;

    for (;;)
    {
        if (!apth_sched_is_opening(sched))
        {
            apth_debug("RECEIVED REQUEST TO BREAK");
            break;
        }

        // For main worker, check whether we should end the process
        if (worker0_check_end_process(me))
        {
            apth_debug("MAIN REQUEST TO END THE PROCESS");
            break;
        }

        apth_debug("new loop");
        // Move all new threads to ready list
        apth_t th;
        while ((th = transfer_one_th(sched->new_queue, sched->ready_queue, false, "transfer_one_th moving new")) != APTH_NULL)
            ;

        apth_debug("moving waked apths");
        while ((th = transfer_one_th(sched->waked_queue, sched->ready_queue, true, "transfer_one_th moving waked")) != APTH_NULL)
            ;

        // Update statistics
        apth_sched_calc_load(sched, &snapshot);

        // Move one apth from ready queue to running queue
        th = transfer_one_th(sched->ready_queue, sched->running_queue, false, "transfer_one_th popping candidate");

        apth_debug("popped apth=%p (\"%s\")", th, th == APTH_NULL ? "" : th->name);

        if (th == APTH_NULL)
        {
            // There is no more thread to run
            // TODO: steal work
            apth_time_set(&snapshot, APTH_TIME_NOW);
            apth_sched_eventmanager_epoll(sched, &snapshot, false /* dopoll */);
            sched_yield();
            continue;
        }

        assert(APTH_IS_VALID(th));
        apth_debug("decided next thread to run: %p (\"%s\")", th, th->name);

        apth_check_process_signals(sched);

        // Set running start time for new thread and perform a context switch to it
        apth_debug("switching to thread %p (\"%s\")", th, th->name);

        // Update thread times
        apth_time_set(&th->lastran, APTH_TIME_NOW);

        // Update scheduler times
        apth_time_set(&running, &th->lastran);
        apth_time_sub(&running, &snapshot);
        apth_time_add(&sched->running, &running);

        // Switch the thread
        th->dispatches += 1;

        // Set current thread
        set_cur_apth(th);

        // Before context switch, we should handle signals
        if (th->sigpendcnt > 0)
            apth_deliver_pending_signals(th);
        apth_ctx_switch(sched->sched_ctx, th->ctx);
        // Prepare for thread insertion and event management phase
        set_cur_apth(APTH_FAKE_SCHED(sched));

        // Update scheduler times
        apth_time_set(&snapshot, APTH_TIME_NOW);
        apth_debug("cameback from thread %p (\"%s\")", th, th->name);

        // Update thread times
        apth_time_set(&running, &snapshot);
        apth_time_sub(&running, &th->lastran);
        apth_time_add(&th->running, &running);
        apth_debug("thread \"%s\" ran %.6f", th->name, apth_time_t2d(&running));

        // Check for stack overflow
        if (th->stackguard != NULL)
        {
            if (*th->stackguard != APTH_MAGIC)
            {
                apth_debug("stack overflow detected for thread %p (\"%s\")", th, th->name);
                // If the application doesn't catch SIGSEGVs, then terminate manually
                // with a SIGSEGV now
                if (apth_syscall_raw(sigaction)(SIGSEGV, NULL, &sa) == 0)
                {
                    if (sa.sa_handler == SIG_DFL)
                    {
                        apth_debug("APTH STACK OVERFLOW: thread pid_t=0x%lx, name=\"%s\"",
                                   (unsigned long)th, th->name);
                        kill(getpid(), SIGSEGV); // Kill to all threads in the whole process
                        sigfillset(&ss);
                        sigdelset(&ss, SIGSEGV);
                        sigsuspend(&ss);
                        abort();
                    }
                }

                // Else terminate the thread only and send a SIGSEGV which allows the application
                // to handle the situation
                th->join_arg = (void *)APTH_MAGIC;
                // Note that we force set the state to TERMINATED
                atomic_store_release(&th->state_holder, make_state_uncommitted(APTH_STATE_TERMINATED));
                apth_kill(th, SIGSEGV);
            }
        }

        apth_state_t retired_th_state = state_holder_of(th);
        switch (make_state_committed(retired_th_state))
        {
        case APTH_STATE_NEW:
            PANIC("Insane");
            break;
        case APTH_STATE_READY:
            PANIC("Insane");
            break;
        case APTH_STATE_RUNNING:
            assert(retired_th_state == APTH_STATE_RUNNING);
            apth_debug("moving thread \"%s\" to ready queue", th->name);
            submit_desired_state_to(th, APTH_STATE_READY, "moving running to ready");
            transfer_th(th, sched->running_queue, sched->ready_queue);
            break;
        case APTH_STATE_WAITING:
            assert(state_is_uncommitted(retired_th_state));
            apth_debug("moving thread \"%s\" to waiting queue", th->name);
            transfer_th(th, sched->running_queue, sched->waiting_queue);
            break;
        case APTH_STATE_TERMINATED:
            assert(state_is_uncommitted(retired_th_state));
            apth_debug("marking thread \"%s\" as terminated", th->name);
            dec_alive_thrcnt(); // decrement alive nthreads
            // NOTE: since `th` is marked as terminated, then all cleanups should
            // have been executed.
            if (IS_DETACHED(th))
            {
                remove_apth_from(sched->running_queue, th);
                apth_tcb_free(th);
            }
            else
                // For other apths to join `th`
                transfer_th(th, sched->running_queue, sched->terminated_queue);
            break;
        default:
            PANIC("Insane");
            break;
        }

        th = APTH_NULL;

        // Manage events in the waiting queue
        if (thqueue_size(sched->ready_queue) == 0 && thqueue_size(sched->new_queue) == 0)
        {
            apth_debug("no NEW or READY threads, have to wait for new work");
            apth_sched_eventmanager_epoll(sched, &snapshot, false /* dopoll */);
        }
        else
        {
            apth_debug("already NEW or READY threads exists, so just poll for even more work");
            apth_sched_eventmanager_epoll(sched, &snapshot, true /* dopoll */);
        }
    }

    apth_debug("WORKER %d(tid=%p) EXITING...", me->worker_id, me->tid);

    if (is_main_worker(me))
    {
        apth_debug("MAIN WORKER %d ENDING THE PROCESS...", me->worker_id, me->tid);

        // First we should isolate ourself from the pool
        lll_lock(&GLOBAL_POOL.pool_lock, "scheduler isolation");
        list_remove(&me->elem);
        lll_unlock(&GLOBAL_POOL.pool_lock, "scheduler isolation");
        apth_drop();
    }

    // Do cleaning
    apth_debug("WORKER %d cleaning self", me->worker_id);
    apth_scheduler_kill();

    // Only worker 0 needs to free itself. Other workers are freed in `apth_drop`
    // routine by worker 0.
    if (is_main_worker(me))
        free(me);

    // Now we should be an ordinary pthread.
    return NULL;
}
