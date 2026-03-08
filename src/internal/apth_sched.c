#ifndef _GNU_SOURCE
#define _GNU_SOURCE // For SIG_SETMASK
#endif
#include <signal.h>

#include "common.h" // For WORKER_SPAWNED
#include "apth_sched.h"
#include "internal/types.h"
#include "internal/apth_event.h"
#include "internal/apth_fd.h"
#include "internal/apth_fd_slot.h"
#include "internal/apth_time.h"
#include "internal/apth_global_sched_pool.h"
#include "internal/apth_thqueue.h"
#include "internal/apth_signal.h"
#include "hook_libc/hooked_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/apth_errno.h"
#include "utils/lll.inline.h"
#include "utils/apth_getpid.h"
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

// Total APTH threads we have. Note this counter is shared across the process,
// So it should be _Atomic.
_Atomic(unsigned int) apth_nthreads = 0;
_Atomic(unsigned int) apth_alive_nthreads = 0;

static _Atomic(apth_t) MAIN_APTH = APTH_NULL;
_Atomic(int) MAIN_APTH_EXITED = 0;
_Atomic(int) MAIN_APTH_EXITED_BY_CALLING_APTH_EXIT = 0;

#ifdef APTH_CUR_USING_KEYWORD
APTH_THREAD_LOCAL apth_sched_t __cur_sched_tls = NULL;
#else
// Use pthread TLS API
pthread_key_t __CUR_SCHED_KEY;
static void sched_key_t_destr_fn(void *) { /* nop */ }
#endif

void sched_key_t_init(void)
{
#ifdef APTH_CUR_USING_KEYWORD
    // No initialization needed for thread-local storage keywords
    __cur_sched_tls = NULL;
#else
    int result = apth_func_raw(pthread_key_create)(&__CUR_SCHED_KEY, sched_key_t_destr_fn);
    assert_msg(result == 0, "fail pthread_key_create");
#endif
}

void sched_key_t_drop(void)
{
#ifdef APTH_CUR_USING_KEYWORD
    __cur_sched_tls = NULL;
#else
    int result = apth_func_raw(pthread_key_delete)(__CUR_SCHED_KEY);
    assert_msg(result == 0, "fail pthread_key_delete");
#endif
}

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

    thqueue_init(THQUEUE(sched, new), APTH_STATE_NEW);
    thqueue_init(THQUEUE(sched, ready), APTH_STATE_READY);
    thqueue_init(THQUEUE(sched, waiting), APTH_STATE_WAITING);
    thqueue_init(THQUEUE(sched, terminated), APTH_STATE_TERMINATED);
    thqueue_init(THQUEUE(sched, waked), APTH_STATE_WAKED);
    thqueue_init(THQUEUE(sched, running), APTH_STATE_RUNNING);

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

    // Create wake eventfd and register it with our epoll so other threads can
    // interrupt a blocking epoll_wait when new work arrives.
    sched->wake_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    apth_fd_register(sched->epoll_fd);
    apth_fd_register(sched->wake_eventfd);

    if (sched->wake_eventfd < 0)
    {
        apth_func_raw(close)(sched->epoll_fd);
        sched->epoll_fd = -1;
        return apth_error(false, errno);
    }
    {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = sched->wake_eventfd;
        if (epoll_ctl(sched->epoll_fd, EPOLL_CTL_ADD, sched->wake_eventfd, &ev) < 0)
        {
            apth_func_raw(close)(sched->wake_eventfd);
            sched->wake_eventfd = -1;
            apth_func_raw(close)(sched->epoll_fd);
            sched->epoll_fd = -1;
            return apth_error(false, errno);
        }
    }

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

    // Initialize pending fd close notification queue
    atomic_store_release(&sched->pending_fd_close_count, 0);
    lll_internal_init(&sched->pending_fd_close_lock);

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

// Wake a scheduler that may be blocked in epoll_wait by writing to its eventfd.
APTH_INTERNAL void apth_sched_wake(apth_sched_t sched)
{
    if (sched->wake_eventfd >= 0)
    {
        uint64_t val = 1;
        // Ignore errors: the scheduler may already be awake, or shutting down.
        ssize_t __ignored = apth_func_raw(write)(sched->wake_eventfd, &val, sizeof(val));
        (void)__ignored;
    }
}

static apth_time_t apth_loadtickgap = APTH_TIME(1, 0);

APTH_INTERNAL void apth_sched_calc_load(apth_sched_t sched, apth_time_t *now)
{
    if (apth_time_cmp(now, &sched->apth_loadticknext) >= 0)
    {
        apth_time_t ttmp;
        int numready = thqueue_size(THQUEUE(sched, ready));
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
    apth_sched_t sched = CUR_SCHED;

    drain_thqueue(THQUEUE(sched, new), __drain_free_th);
    drain_thqueue(THQUEUE(sched, ready), __drain_free_th);
    drain_thqueue(THQUEUE(sched, waiting), __drain_free_th);
    drain_thqueue(THQUEUE(sched, terminated), __drain_free_th);
    drain_thqueue(THQUEUE(sched, waked), __drain_free_th);
    drain_thqueue(THQUEUE(sched, running), __drain_free_th);

    // free(sched->new_queue);
    // free(sched->ready_queue);
    // free(sched->waiting_queue);
    // free(sched->terminated_queue);
    // free(sched->waked_queue);
    // free(sched->running_queue);

    // TODO: report scheduler statistics if in debugging mode

    // Drop wake eventfd (before epoll_fd so it's automatically removed from epoll)
    if (sched->wake_eventfd >= 0)
    {
        apth_func_raw(close)(sched->wake_eventfd);
        sched->wake_eventfd = -1;
    }

    // Drop epoll
    if (sched->epoll_fd >= 0)
    {
        // Use raw close to avoid recursion
        apth_func_raw(close)(sched->epoll_fd);
        sched->epoll_fd = -1;
    }

    // Waiters in active_fd_slots and fd_slot_table should have been cleared
    // in drain_thqueue process and memory freed. No more clean to do.

    // Signal mask restore, allow all signals
    sigset_t sigs;
    sigemptyset(&sigs);
    apth_func_raw(pthread_sigmask)(SIG_SETMASK, &sigs, NULL);

    // Cancel TLS, no need for set current apth to APTH_NULL
    // since clearing scheduler will do this.
    SET_CUR_SCHED(NULL);
    sched_key_t_drop();
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

// Try to steal one thread from another scheduler's ready queue.
// Returns the stolen apth_t on success, or APTH_NULL if nothing was stolen.
//
// Protocol (follows the same lock-unlock-relock-commit pattern as transfer_one_th):
//   1. Lock victim's ready_queue -> pop_back -> dec size -> unlock
//   2. submit_desired_state_to(th, APTH_STATE_READY)
//   3. Lock thief's ready_queue -> push_front -> inc size ->
//      set_belonging_queue -> commit_state -> unlock
//   4. Adjust thrcnt for both schedulers
static apth_t try_steal_work(apth_sched_t thief_sched)
{
    int n_workers = GLOBAL_POOL.init_worker_count;
    if (n_workers <= 1)
        return APTH_NULL;

    // Vary start offset to distribute steal attempts and avoid thundering herd
    unsigned int offset = thief_sched->switches;

    for (int i = 0; i < n_workers; i++)
    {
        int victim_id = (int)((offset + (unsigned int)i) % (unsigned int)n_workers);

        // Don't steal from ourselves
        if (victim_id == thief_sched->id)
            continue;

        apth_worker_t victim_worker = GLOBAL_POOL.worker_ptr_mem_start[victim_id];
        if (victim_worker == NULL)
            continue;

        apth_sched_t victim_sched = victim_worker->sched;
        if (victim_sched == NULL || !apth_sched_is_opening(victim_sched))
            continue;

        // Speculative lock-free check: skip if victim has 0 or 1 ready threads
        if (thqueue_size(THQUEUE(victim_sched, ready)) <= 1)
            continue;

        // Attempt to steal from the BACK of victim's ready queue
        apth_thqueue_t victim_rq = THQUEUE(victim_sched, ready);

        lll_internal_lock(&victim_rq->th_list_lock);

        // Re-check under lock
        // if (list_empty(&victim_rq->th_list) || atomic_load_acquire(&victim_rq->size) <= 1)
        if (list_empty(&victim_rq->th_list) || victim_rq->size <= 1)
        {
            lll_internal_unlock(&victim_rq->th_list_lock);
            continue;
        }

        // Inspect the back apth
        struct list_elem *e = list_back(&victim_rq->th_list);
        apth_t th = apth_t_list_entry(e);

        // If the `th` happens to be the advised thread, then we just cancel this stealing
        // and inspect next scheduler. If all stealings fails we natually fails.
        if (th == atomic_load_acquire(&victim_sched->advised_next_th))
        {
            lll_internal_unlock(&victim_rq->th_list_lock);
            continue;
        }

        // Steal from back (owner dispatches from front, thief steals from back)
        e = list_pop_back(&victim_rq->th_list);
        // atomic_fetch_sub_release(&victim_rq->size, 1);
        victim_rq->size--;

        // Acquire ownership lock before releasing victim queue lock
        // This ensures atomic ownership transfer
        lll_internal_lock(&th->ownership_lock);

        lll_internal_unlock(&victim_rq->th_list_lock);

        th = apth_t_list_entry(e);
        assert(APTH_IS_VALID(th));
        assert(th->current_queue == victim_rq);

        // Update ownership: this APTH now belongs to thief scheduler
        th->current_sched = thief_sched;

        // Update state to READY
        atomic_store_release(&th->state, APTH_STATE_READY);

        // Insert into thief's ready queue at front for immediate dispatch
        apth_thqueue_t thief_rq = THQUEUE(thief_sched, ready);

        lll_internal_lock(&thief_rq->th_list_lock);
        list_push_front(&thief_rq->th_list, &th->elem);
        // atomic_fetch_add_release(&thief_rq->size, 1);
        thief_rq->size++;
        // set_belonging_queue_of(th, thief_rq);
        th->current_queue = thief_rq; // Update current_queue
        lll_internal_unlock(&thief_rq->th_list_lock);

        // Release ownership lock
        lll_internal_unlock(&th->ownership_lock);

        // Adjust per-scheduler thread counts
        dec_thrcnt(victim_sched);
        inc_thrcnt(thief_sched);

        apth_debug("stole thread %p (\"%s\") from sched %d", th, th->name, victim_id);
        return th;
    }

    return APTH_NULL;
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
    if ((sched = (apth_sched_t)malloc(sizeof(struct apth_sched_st))) == NULL)
        return apth_error(NULL, ENOMEM);
    if (!apth_scheduler_init(sched, me))
    {
        // Fail to initialize
        return apth_error(NULL, errno);
    }

    apth_debug("WORKER %d created scheduler", me->worker_id);

    // Set TLS
    // set_cur_worker(me);
    SET_CUR_SCHED(sched);
    SET_CUR_APTH(APTH_FAKE_SCHED(sched));

    sigset_t sigs;
    apth_time_t snapshot;
    apth_time_t running;

    // block all signals in the scheduler thread
    sigfillset(&sigs);
    apth_func_raw(pthread_sigmask)(SIG_SETMASK, &sigs, NULL);

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
        while ((th = transfer_one_th(THQUEUE(sched, new), THQUEUE(sched, ready), false, "transfer_one_th moving new")) != APTH_NULL)
            ;

        apth_debug("moving waked apths");
        while ((th = transfer_one_th(THQUEUE(sched, waked), THQUEUE(sched, ready), true, "transfer_one_th moving waked")) != APTH_NULL)
            ;

        // Update statistics
        apth_sched_calc_load(sched, &snapshot);

        // If there's advised apth (which is not WAITING, but rather urgent lll waiting one!)
        // Schedule it right now
        // Use atomic_exchange to load and clear in one operation to prevent reusing stale advice
        th = atomic_exchange_acqrel(&sched->advised_next_th, APTH_NULL);
        if (th != APTH_NULL)
        {
            // The advised thread might have been freed, stolen, or changed state
            // Validate before using it
            if (APTH_IS_VALID(th) && SCHED_OF(th) == sched && QUEUE_STATE_OF(th) == APTH_STATE_READY)
            {
                atomic_store_release(&th->state, APTH_STATE_RUNNING);
                transfer_th(th, th->current_queue, THQUEUE(sched, running));
            }
            else
                // Thread was freed, stolen, or not ready - ignore the advice and pop from ready queue
                th = APTH_NULL;
        }

        if (th == APTH_NULL)
            // Move one apth from ready queue to running queue
            th = transfer_one_th(THQUEUE(sched, ready), THQUEUE(sched, running), false, "transfer_one_th popping candidate");

        apth_debug("popped apth=%p (\"%s\")", th, th == APTH_NULL ? "" : th->name);

        if (th == APTH_NULL)
        {
            // Try to steal work from another scheduler's ready queue
            apth_t stolen = try_steal_work(sched);
            if (stolen != APTH_NULL)
            {
                // Stolen thread is at front of our ready queue; loop back to dispatch it
                apth_debug("work stolen, re-entering loop");
                continue;
            }
            // No work to steal; block in epoll_wait via the event manager
            apth_time_set(&snapshot, APTH_TIME_NOW);
            apth_sched_eventmanager_epoll(sched, &snapshot, false /* dopoll */);
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
        SET_CUR_APTH(th);

        // Restore yield reason to VOLUNTEER since program code would not do this
        // for us, but we could set reason before yielding within LIBAPTH.
        th->yield_reason = APTH_YIELD_REASON_VOLUNTEER;

        // Before context switch, we should handle signals
        if (th->sigpendcnt > 0)
            apth_deliver_pending_signals(th);
        apth_ctx_switch(SCHED_CTX(sched), CTX(th));
        // Prepare for thread insertion and event management phase
        SET_CUR_APTH(APTH_FAKE_SCHED(sched));

        // Update scheduler times
        apth_time_set(&snapshot, APTH_TIME_NOW);
        apth_debug("cameback from thread %p (\"%s\")", th, th->name);

        // Update thread times
        apth_time_set(&running, &snapshot);
        apth_time_sub(&running, &th->lastran);
        apth_time_add(&th->running, &running);
        apth_debug("thread \"%s\" ran %.6f", th->name, apth_time_t2d(&running));

        // Note: Stack overflow detection is now handled by guard pages.
        // If a thread overflows its stack, it will trigger a SIGSEGV when
        // accessing the protected guard page, which is handled by the OS.

        // Check yield reason for EXIT (TERMINATED transition)
        // For TERMINATED, we handle it specially to ensure atomicity
        if (th->yield_reason == APTH_YIELD_REASON_EXIT)
        {
            apth_debug("marking thread \"%s\" as terminated", th->name);
            dec_alive_thrcnt(); // decrement alive nthreads

            // NOTE: since `th` is marked as terminated, then all cleanups should
            // have been executed.
            if (IS_DETACHED(th))
            {
                // For detached threads, just remove and free
                remove_apth_from(THQUEUE(sched, running), th);
                atomic_store_release(&th->state, APTH_STATE_TERMINATED);
                apth_tcb_free(th);
            }
            else
            {
                // For joinable threads, transfer to terminated queue
                // and set state WHILE HOLDING the terminated queue lock
                apth_thqueue_t running_q = THQUEUE(sched, running);
                apth_thqueue_t term_q = THQUEUE(sched, terminated);

                lll_internal_lock(&running_q->th_list_lock);
                lll_internal_lock(&term_q->th_list_lock);

                // Remove from running queue
                list_remove(&th->elem);
                running_q->size--;
                // set_belonging_queue_of(th, NULL);
                th->current_queue = NULL;

                // Insert into terminated queue
                list_push_back(&term_q->th_list, &th->elem);
                term_q->size++;
                // set_belonging_queue_of(th, term_q);
                th->current_queue = term_q;

                // Change state WHILE HOLDING terminated queue lock
                // This ensures atomicity of "state change + queue insertion"
                atomic_store_release(&th->state, APTH_STATE_TERMINATED);

                lll_internal_unlock(&term_q->th_list_lock);
                lll_internal_unlock(&running_q->th_list_lock);
            }
        }
        else
        {
            // Handle other state transitions
            apth_state_t retired_th_state = atomic_load_acquire(&th->state);
            switch (retired_th_state)
            {
            case APTH_STATE_NEW:
                PANIC("Insane");
                break;
            case APTH_STATE_READY:
                PANIC("Insane");
                break;
            case APTH_STATE_RUNNING:
                apth_debug("moving thread \"%s\" to ready queue", th->name);
                atomic_store_release(&th->state, APTH_STATE_READY);
                transfer_th(th, THQUEUE(sched, running), THQUEUE(sched, ready));
                break;
            case APTH_STATE_WAITING:
                apth_debug("moving thread \"%s\" to waiting queue", th->name);
                transfer_th(th, THQUEUE(sched, running), THQUEUE(sched, waiting));
                break;
            case APTH_STATE_TERMINATED:
                // This case should not happen anymore since we handle EXIT via yield reason
                PANIC("TERMINATED state should be handled via YIELD_REASON_EXIT");
                break;
            default:
                PANIC("Insane");
                break;
            }
        }

        th = APTH_NULL;

        // Manage events in the waiting queue
        if (thqueue_size(THQUEUE(sched, ready)) == 0 && thqueue_size(THQUEUE(sched, new)) == 0)
        {
            // Try stealing before blocking
            apth_t stolen = try_steal_work(sched);
            if (stolen == APTH_NULL)
            {
                apth_debug("no NEW or READY threads, have to wait for new work");
                apth_sched_eventmanager_epoll(sched, &snapshot, false /* dopoll */);
            }
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
        lll_internal_lock(&GLOBAL_POOL.pool_lock);
        list_remove(&me->elem);
        lll_internal_unlock(&GLOBAL_POOL.pool_lock);
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
