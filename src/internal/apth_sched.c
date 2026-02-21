#include "common.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/apth_errno.h"
#include "utils/lll.h"
#include <stdlib.h>

// Total APTH threads we have. Note this counter is shared across the process,
// So it should be _Atomic.
_Atomic unsigned int apth_nthreads = 0;
_Atomic unsigned int apth_alive_nthreads = 0;

static _Atomic apth_t MAIN_APTH = APTH_NULL;

_Atomic int MAIN_APTH_EXITED = 0;
_Atomic int MAIN_APTH_EXITED_BY_CALLING_APTH_EXIT = 0;

// Whichever worker wants to drop the whole pool should acquire this permit first
// _Atomic unsigned int DROP_POOL_PERMIT = 1;

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
    sched->id = worker->worker_id;

    // Initialize scheduler context
    sched->sched_ctx = apth_ctx_alloc();
    if (sched->sched_ctx == NULL)
        return apth_error(false, ENOMEM);

    list_init(&sched->new_list);
    list_init(&sched->ready_list);
    list_init(&sched->waiting_list);
    list_init(&sched->suspended_list);
    list_init(&sched->terminated_list);

    lll_init(&sched->new_list_lock);
    lll_init(&sched->ready_list_lock);
    lll_init(&sched->waiting_list_lock);
    lll_init(&sched->suspended_list_lock);
    lll_init(&sched->terminated_list_lock);

    sched->worker = worker;
    sched->switches = 0;
    sched->thrcnt = 0;
    apth_time_set(&sched->running, APTH_TIME_ZERO);
    sched->cur = APTH_NULL;

    if (apth_syscall_raw(pipe)(sched->apth_sigpipe) != 0)
        return apth_error(false, errno);
    if (apth_fdmode(sched->apth_sigpipe[0], APTH_FDMODE_NONBLOCK) == APTH_FDMODE_ERROR)
        return apth_error(false, errno);
    if (apth_fdmode(sched->apth_sigpipe[1], APTH_FDMODE_NONBLOCK) == APTH_FDMODE_ERROR)
        return apth_error(false, errno);

    sigemptyset(&sched->apth_sigpending);
    sigemptyset(&sched->apth_sigblock);
    sigemptyset(&sched->apth_sigcatch);
    sigemptyset(&sched->apth_sigraised);

    // Initialize load support
    apth_time_set(&sched->apth_loadticknext, APTH_TIME_NOW);
    sched->loadval = 1.0;

    // Store the sched into the worker
    worker->sched = sched;

    // Mark the scheduler as opening
    atomic_store_release(&sched->opening, true);
    fprintf(stderr, "(%d) apth_scheduler_init: leave\n", sched->id);
    return true;
}

APTH_INTERNAL void inc_thrcnt(apth_sched_t sched)
{
    // sched->thrcnt += 1;
    atomic_fetch_add_release(&sched->thrcnt, 1);
    atomic_fetch_add_release(&apth_nthreads, 1);
}

APTH_INTERNAL void dec_thrcnt(apth_sched_t sched)
{
    // unsigned int c = sched->thrcnt;
    // sched->thrcnt = c == 0 ? c : c - 1;
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

#define push_apth_to(name) push_apth_to_##name
#define pop_apth_from(name) pop_apth_from_##name
#define head_apth_of(name) head_apth_of_##name
#define apth_is_in(name) apth_is_in_##name
#define list_of(name) name##_list
#define list_lock_of(name) name##_list_lock
// TODO: acquire list lock, since the caller pthread may not be ourself
// TODO: release list lock
#define DEFINE_SCHED_LIST_OP(name)                                                \
    APTH_INTERNAL void push_apth_to(name)(apth_t th, apth_sched_t sched)          \
    {                                                                             \
        assert(APTH_IS_VALID(th));                                                \
        assert(th->belongs_to_list == NULL);                                      \
        assert(th->belongs_to_list_lock == NULL);                                 \
        lll_lock(&sched->list_lock_of(name), "push_apth_to_" stringify(name));    \
        list_push_back(&sched->list_of(name), &th->elem);                         \
        lll_unlock(&sched->list_lock_of(name), "push_apth_to_" stringify(name));  \
        th->worker = sched->worker;                                               \
        th->belongs_to_list = &sched->list_of(name);                              \
        th->belongs_to_list_lock = &sched->list_lock_of(name);                    \
    }                                                                             \
    APTH_INTERNAL apth_t pop_apth_from(name)(apth_sched_t sched)                  \
    {                                                                             \
        apth_t th = APTH_NULL;                                                    \
        struct list_elem *e;                                                      \
        lll_lock(&sched->list_lock_of(name), "pop_apth_from_" stringify(name));   \
        if (!list_empty(&sched->list_of(name)))                                   \
        {                                                                         \
            e = list_pop_front(&sched->list_of(name));                            \
            th = apth_t_list_entry(e);                                            \
            assert(th->belongs_to_list == &sched->list_of(name));                 \
            assert(th->belongs_to_list_lock == &sched->list_lock_of(name));       \
            th->belongs_to_list = NULL;                                           \
            th->belongs_to_list_lock = NULL;                                      \
        }                                                                         \
        lll_unlock(&sched->list_lock_of(name), "pop_apth_from_" stringify(name)); \
        return th;                                                                \
    }                                                                             \
    APTH_INTERNAL apth_t head_apth_of(name)(apth_sched_t sched)                   \
    {                                                                             \
        apth_t th = NULL;                                                         \
        struct list_elem *e;                                                      \
        lll_lock(&sched->list_lock_of(name), "head_apth_of_" stringify(name));    \
        if (!list_empty(&sched->list_of(name)))                                   \
        {                                                                         \
            e = list_front(&sched->list_of(name));                                \
            th = apth_t_list_entry(e);                                            \
        }                                                                         \
        lll_unlock(&sched->list_lock_of(name), "head_apth_of_" stringify(name));  \
        return th;                                                                \
    }                                                                             \
    APTH_INTERNAL bool apth_is_in(name)(apth_t t, apth_sched_t sched)             \
    {                                                                             \
        bool found = false;                                                       \
        lll_lock(&sched->list_lock_of(name), "apth_is_in_" stringify(name));      \
        FOR_ELEMENT_IN_LIST_REF(&sched->list_of(name), e)                         \
        {                                                                         \
            apth_t th = apth_t_list_entry(e);                                     \
            if (t == th)                                                          \
                found = true;                                                     \
        }                                                                         \
        lll_unlock(&sched->list_lock_of(name), "apth_is_in_" stringify(name));    \
        return found;                                                             \
    }

// Wait `th` to be in a list.
APTH_INTERNAL void wait_apth_to_be_in_list(apth_t th)
{
    while (atomic_load_acquire(&th->belongs_to_list) == NULL)
        ;
    while (atomic_load_acquire(&th->belongs_to_list_lock) == NULL)
        ;
}

APTH_INTERNAL void remove_apth(apth_t th)
{
    assert(APTH_IS_VALID(th));
    assert(th->belongs_to_list != NULL);
    assert(th->belongs_to_list_lock != NULL);
    lll_lock(th->belongs_to_list_lock, "remove_apth");
    list_remove(&th->elem);
    lll_unlock(th->belongs_to_list_lock, "remove_apth");
    th->belongs_to_list = NULL;
    th->belongs_to_list_lock = NULL;
}

DEFINE_SCHED_LIST_OP(new)
DEFINE_SCHED_LIST_OP(ready)
DEFINE_SCHED_LIST_OP(waiting)
DEFINE_SCHED_LIST_OP(suspended)
DEFINE_SCHED_LIST_OP(terminated)

#undef DEFINE_SCHED_LIST_OP
#undef list_lock_of
#undef list_of
#undef apth_is_in
#undef head_apth_of
#undef pop_apth_from
#undef push_apth_to

APTH_INTERNAL bool apth_sched_is_opening(apth_sched_t sched)
{
    // fprintf(stderr, "(%d) apth_sched_is_opening enter\n", sched->id);
    return atomic_load_acquire(&sched->opening);
}

static apth_time_t apth_loadtickgap = APTH_TIME(1, 0);

APTH_INTERNAL void apth_sched_calc_load(apth_sched_t sched, apth_time_t *now)
{
    // apth_debug("enter");
    if (apth_time_cmp(now, &sched->apth_loadticknext) >= 0)
    {
        apth_time_t ttmp;
        int numready = list_size(&sched->ready_list);
        apth_time_set(&ttmp, now);
        do
        {
            sched->loadval = (numready * 0.25) + (sched->loadval * 0.75);
            apth_time_sub(&ttmp, &apth_loadtickgap);
        } while (apth_time_cmp(&ttmp, &sched->apth_loadticknext) >= 0);
        apth_time_set(&sched->apth_loadticknext, now);
        apth_time_add(&sched->apth_loadticknext, &apth_loadtickgap);
    }
    // apth_debug("leave");
}

// Kill the schduler ingredients
APTH_INTERNAL void apth_scheduler_kill(void)
{
    apth_sched_t sched = cur_sched();

// Drop all apths
// TODO: do not know whether should apth_thread_cleanup(t);
#define CLEAR_T_LIST(name)                           \
    FOR_ELEMENT_IN_LIST(sched->name##_list, e##name) \
    {                                                \
        apth_t t = apth_t_list_entry(e##name);       \
        t->state = APTH_STATE_TERMINATED;            \
        if (t->join_arg == NULL)                     \
        {                                            \
            t->join_arg = APTH_CANCELED;             \
        }                                            \
        apth_thread_cleanup(t);                      \
        apth_tcb_free(t);                            \
    }                                                \
    list_init(&sched->name##_list);

    // Clear the apth queues
    CLEAR_T_LIST(new);
    CLEAR_T_LIST(ready);
    CLEAR_T_LIST(waiting);
    CLEAR_T_LIST(suspended);
    CLEAR_T_LIST(terminated);
#undef CLEAR_T_LIST
    return;

    // TODO: report scheduler statistics if in debugging mode

    // Signal mask restore, allow all signals
    sigset_t sigs;
    sigemptyset(&sigs);
    apth_syscall_raw(pthread_sigmask)(SIG_SETMASK, &sigs, NULL);

    // Remove the internal signal pipe
    // TODO: should `close` be wrapped by `apth_syscall_raw`?
    close(sched->apth_sigpipe[0]);
    close(sched->apth_sigpipe[1]);

    // Cancel TLS
    set_cur_apth(APTH_NULL);
    set_cur_sched(NULL);
    set_cur_worker(NULL);

    // TODO: pthread_key_t should drop

    // Free sched
    free((void *)sched->sched_ctx);
    free((void *)sched);
    return;
}

APTH_INTERNAL bool apth_is_not_null_and_valid(apth_t th)
{
    return APTH_IS_VALID(th);
    // if (th == APTH_NULL)
    //     return false;

    // // Assert sanity
    // apth_sched_t sched_of_th = th->worker->sched;
    // struct list *sl = NULL;
    // switch (th->state)
    // {
    // case APTH_STATE_NEW:
    //     apth_debug("NEW LIST");
    //     sl = &sched_of_th->new_list;
    //     break;
    // case APTH_STATE_READY:
    //     apth_debug("READY LIST");
    //     sl = &sched_of_th->ready_list;
    //     break;
    // case APTH_STATE_WAITING:
    //     apth_debug("WAITING LIST");
    //     sl = &sched_of_th->waiting_list;
    //     break;
    // case APTH_STATE_TERMINATED:
    //     apth_debug("TERMINATED LIST");
    //     sl = &sched_of_th->terminated_list;
    //     break;
    // default:
    //     PANIC("should not reach here");
    //     break;
    // }
    // struct list *l = th->belongs_to_list;

    // assert_msg(l == sl && l != NULL, "l = %p, sl = %p", l, sl);

    // lll_t *lll = th->belongs_to_list_lock;
    // lll_lock(lll, "apth_is_not_null_and_valid");

    // // List contains
    // bool found = false;

    // FOR_ELEMENT_IN_LIST_REF(l, e)
    // {
    //     apth_t t = apth_t_list_entry(e);
    //     if (t == th)
    //         found = true;
    // }

    // lll_unlock(lll, "apth_is_not_null_and_valid");
    // return found;
}

static inline bool is_main_worker(apth_worker_t worker)
{
    return worker->worker_id == 0;
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
        // else: Other threads are also running, we cannot end.
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

    fprintf(stderr, "worker(%d) entered routine\n", me->worker_id);

    // Allocate and initialize the scheduler
    apth_sched_t sched;
    if ((sched = (apth_sched_t)malloc(sizeof(struct apth_perpthr_scheduler))) == NULL)
        return apth_error(NULL, ENOMEM);
    if (!apth_scheduler_init(sched, me))
    {
        // Fail to initialize
        return apth_error(NULL, errno);
    }

    fprintf(stderr, "worker(%d) created scheduler\n", me->worker_id);

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

    // bool end_the_process = false;
    // while (apth_sched_is_opening(sched))
    for (;;)
    {
        // apth_debug("WORKER (%d) open ? %s", me->worker_id,still_opening ? "yes" : "no");
        if (!apth_sched_is_opening(sched))
        {
            apth_debug("(%d) RECEIVED REQUEST TO BREAK", me->worker_id);
            break;
        }

        // For main worker, check whether we should end the process
        if (worker0_check_end_process(me))
        {
            apth_debug("(%d) MAIN REQUEST TO END THE PROCESS", me->worker_id);
            break;
        }

        apth_debug("(%d) new loop", sched->id);
        // Move all new threads to ready list
        apth_t th;
        while ((th = pop_apth_from_new(sched)) != APTH_NULL)
        {
            apth_debug("(%d) move from new to ready: %p (\"%s\")", sched->id, th, th->name);
            th->state = APTH_STATE_READY;
            // TODO: insert into ready queue according to policy
            // TODO: here just append
            push_apth_to_ready(th, sched);
        }

        // Update statistics
        apth_sched_calc_load(sched, &snapshot);

        // Find the next thread in ready queue and set it to be run
        th = pop_apth_from_ready(sched);
        // apth_debug("(%d) popped apth=%p (\"%s\")", sched->id, th, th == APTH_NULL ? "" : th->name);

        if (th == APTH_NULL)
        {
            // there is no more thread to ready, panic
            // PANIC("APTH SCHEDULER INTERNAL ERROR: no more threads available to schedule");

            // Check alive nthreads
            // if (get_apth_alive_nthreads() == 0)
            //     break; // Now we should exit
            // TODO: steal work

            // apth_debug("(%d) IDLE...", sched->id);

            apth_time_set(&snapshot, APTH_TIME_NOW);
            apth_sched_eventmanager(sched, &snapshot, false /* dopoll */);
            sched_yield();
            continue;
        }

        assert(APTH_IS_VALID(th));
        apth_debug("(%d) decided next thread to run: %p (\"%s\")", sched->id, th, th->name);

        // Set current thread and TCB to TCB, now using TCB is enough
        set_cur_apth(th);

        // Handle signals
        if (th->sigpendcnt > 0)
        {
            sigpending(&sched->apth_sigpending);
            for (int sig = 1; sig < APTH_NSIG; sig++)
            {
                if (sigismember(&th->sigpending, sig) && !sigismember(&sched->apth_sigpending, sig))
                {
                    apth_syscall_raw(pthread_kill)(apth_syscall_raw(pthread_self)(), sig);
                }
            }
        }

        // Set running start time for new thread and perform a context switch to it
        apth_debug("(%d) switching to thread %p (\"%s\")", sched->id, th, th->name);

        // Update thread times
        apth_time_set(&th->lastran, APTH_TIME_NOW);

        // Update scheduler times
        apth_time_set(&running, &th->lastran);
        apth_time_sub(&running, &snapshot);
        apth_time_add(&sched->running, &running);

        // Switch the thread
        th->dispatches += 1;
        // apth_debug("GOING TO SWITCH");
        if (sched->sched_ctx == NULL)
        {
            fprintf(stderr, "(%d) SCHED_CTX is NULL\n", sched->id);
        }
        apth_ctx_switch(sched->sched_ctx, th->ctx);

        // Update scheduler times
        apth_time_set(&snapshot, APTH_TIME_NOW);
        apth_debug("(%d) cameback from thread %p (\"%s\")", sched->id, th, th->name);

        // Update thread times
        apth_time_set(&running, &snapshot);
        apth_time_sub(&running, &th->lastran);
        apth_time_add(&th->running, &running);
        apth_debug("(%d) thread \"%s\" ran %.6f", sched->id, th->name, apth_time_t2d(&running));

        // Handle signals
        if (th->sigpendcnt > 0)
        {
            sigset_t sigstillpending;
            sigpending(&sigstillpending);
            for (int sig = 1; sig < APTH_NSIG; sig++)
            {
                if (sigismember(&th->sigpending, sig))
                {
                    if (!sigismember(&sigstillpending, sig))
                    {
                        // already handled by the apth scheduled just now, remove
                        sigdelset(&th->sigpending, sig);
                        th->sigpendcnt--;
                    }
                    else if (!sigismember(&sched->apth_sigpending, sig))
                    {
                        // The signal is really a signal pending in the worker's pthread
                        // signal set, and `th` has it, but the scheduler do not have
                        // knowledge about it.
                        // A new signal arrives during the apth is scheduled to run
                        // and it happens to be in the signal pending set of `th`.
                        // We must re-deliver this signal for `th` the next time
                        // it is going to be scheduled. Since re-deliver an existing signal
                        // won't trigger anything, we must first delete the signal at
                        // pthread level, and later the scheduler will pthread_kill the
                        // signal according to `th->sigpending` the next time `th` is
                        // scheduled.
                        apth_util_sigdelete(sig);
                    }
                }
            }
        }

        // Check for stack overflow
        if (th->stackguard != NULL)
        {
            if (*th->stackguard != APTH_MAGIC)
            {
                apth_debug("(%d), stack overflow detected for thread %p (\"%s\")",
                           sched->id, th, th->name);
                // If the application doesn't catch SIGSEGVs, then terminate manually
                // with a SIGSEGV now
                if (sigaction(SIGSEGV, NULL, &sa) == 0)
                {
                    if (sa.sa_handler == SIG_DFL)
                    {
                        fprintf(stderr, "APTH STACK OVERFLOW: thread pid_t=0x%lx, name=\"%s\"\n",
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
                th->state = APTH_STATE_TERMINATED;
                apth_syscall_raw(pthread_kill)(apth_syscall_raw(pthread_self)(), SIGSEGV);
            }
        }

        // If previous thread is now marked as dead, kick it out
        if (th->state == APTH_STATE_TERMINATED)
        {
            apth_debug("(%d) marking thread \"%s\" as terminated", sched->id, th->name);

            // decrement alive nthreads
            dec_alive_thrcnt();

            // NOTE: since `th` is marked as terminated, then all cleanups should
            // have been executed.
            if (IS_DETACHED(th))
                apth_tcb_free(th);
            else
                // For other apths to join `th`
                push_apth_to_terminated(th, sched);
            set_cur_apth(APTH_FAKE_SCHED(sched));
            th = APTH_NULL;
        }

        // If thread wants to wait for an event, move it to waiting queue now
        if (th != APTH_NULL && th->state == APTH_STATE_WAITING)
        {
            apth_debug("(%d), moving thread \"%s\", to waiting queue", sched->id, th->name);
            push_apth_to_waiting(th, sched);
            set_cur_apth(APTH_FAKE_SCHED(sched));
            th = APTH_NULL;
        }

        // Insert thread back to ready queue if not inserted into waiting queue
        // TODO: migrate old threads in ready queue into higher prio?
        if (th != APTH_NULL)
        {
            push_apth_to_ready(th, sched);
            set_cur_apth(APTH_FAKE_SCHED(sched));
            th = APTH_NULL;
        }

        // if (get_apth_alive_nthreads() == 0)
        //     break;

        // Manage events in the waiting queue

        // TODO: checking list empty should be protected if work stealing is implemented
        // #ifdef _DEBUG_EVENT_MANAGER
        if (list_empty(&sched->ready_list) && list_empty(&sched->new_list))
        {
            apth_debug("(%d) no NEW or READY threads, have to wait for new work", sched->id);
            apth_sched_eventmanager(sched, &snapshot, false /* dopoll */);
        }
        else
        {
            apth_debug("(%d) already NEW or READY threads exists, so just poll for even more work", sched->id);
            apth_sched_eventmanager(sched, &snapshot, true /* dopoll */);
        }
        // #endif // _DEBUG_EVENT_MANAGER
    }

    // fprintf(stderr, "WORKER %d(tid=%p) EXITING...\n", me->worker_id, me->tid);
    apth_debug("WORKER %d(tid=%p) EXITING...", me->worker_id, me->tid);

    if (is_main_worker(me))
    {
        apth_debug("MAIN WORKER %d ENDING THE PROCESS...", me->worker_id, me->tid);
        // unsigned int expected = 1;
        // unsigned int desired = 0;
        // If current value == `expected`, then update it to `desired` and return true;
        // Else update `expected` as actual value that `mem` holds, and return false.
        // if (atomic_compare_exchange_strong(&DROP_POOL_PERMIT, &expected, desired))
        // {
        // We acquired the permit to drop the pool
        apth_debug("WORKER %d ACQUIRED PERMIT TO DROP THE POOL", me->worker_id);

        // First we should isolate ourself from the pool
        lll_lock(&GLOBAL_POOL.pool_lock, "scheduler isolation");
        list_remove(&me->elem);
        lll_unlock(&GLOBAL_POOL.pool_lock, "scheduler isolation");
        apth_global_scheduler_pool_drop();
        // }
    }

    // Do cleaning
    apth_debug("WORKER %d cleaning self", me->worker_id);
    apth_scheduler_kill();
    // Only worker 0 needs to free itself. Other workers are freed in
    // `apth_global_scheduler_pool_drop` routine.
    if (is_main_worker(me))
        free(me);

    // Now we should be an ordinary pthread.
    // Not reached
    return NULL;
}
