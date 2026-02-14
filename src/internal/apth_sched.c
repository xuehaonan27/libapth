#include "common.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/apth_errno.h"

// Total APTH threads we have. Note this counter is shared across the process,
// So it should be _Atomic.
_Atomic unsigned int apth_nthreads = 0;

// Initialize a scheduler onto `worker` and put the new scheduler in `sched`.
static bool apth_scheduler_init(apth_sched_t sched, apth_worker_t worker)
{
    sched->id = worker->worker_id;

    if (pipe(sched->apth_sigpipe) == -1)
        return apth_error(false, errno);
    if (apth_fdmode(sched->apth_sigpipe[0], APTH_FDMODE_NONBLOCK) == APTH_FDMODE_ERROR)
        return apth_error(false, errno);
    if (apth_fdmode(sched->apth_sigpipe[1], APTH_FDMODE_NONBLOCK) == APTH_FDMODE_ERROR)
        return apth_error(false, errno);

    list_init(&sched->new_list);
    list_init(&sched->ready_list);
    list_init(&sched->waiting_list);
    list_init(&sched->suspended_list);
    list_init(&sched->terminated_list);
    sched->worker = worker;
    sched->switches = 0;
    sched->thrcnt = 0;
    apth_time_set(&sched->running, APTH_TIME_ZERO);
    sched->cur = APTH_NULL;

    // Initialize load support
    sched->loadval = 1.0;
    apth_time_set(&sched->apth_loadticknext, APTH_TIME_NOW);

    // Mark the scheduler as opening
    atomic_store_release(&sched->opening, true);
}

static void inc_thrcnt(apth_sched_t sched)
{
    sched->thrcnt += 1;
    // TODO: increment apth_nthreads
    atomic_fetch_add_relaxed(&apth_nthreads, 1);
}

static void dec_thrcnt(apth_sched_t sched)
{
    unsigned int c = sched->thrcnt;
    sched->thrcnt = c == 0 ? c : c - 1;
    // TODO: decrement apth_nthreads

    unsigned int expected = atomic_load_acquire(&apth_nthreads);
    unsigned int desired = expected == 0 ? expected : expected - 1;
    while (atomic_compare_and_exchange_bool_acq(&apth_nthreads, expected, desired) != 0)
    {
        expected = atomic_load_acquire(&apth_nthreads);
    }
}

#define push_apth_to(name) push_apth_to_##name
#define pop_apth_from(name) pop_apth_from_##name
#define head_apth_of(name) head_apth_of_##name
#define new_apth_to(name) new_apth_to_##name
#define list_of(name) name##_list
// TODO: acquire list lock, since the caller pthread may not be ourself
// TODO: release list lock
#define DEFINE_SCHED_LIST_OP(name)                            \
    void push_apth_to(name)(apth_t th, apth_sched_t sched)    \
    {                                                         \
        assert(th->belongs_to_list == NULL);                  \
        list_push_back(&sched->list_of(name), &th->elem);     \
        th->belongs_to_list = &sched->list_of(name);          \
    }                                                         \
    apth_t pop_apth_from(name)(apth_sched_t sched)            \
    {                                                         \
        apth_t th = NULL;                                     \
        struct list_elem *e;                                  \
        assert(th->belongs_to_list == &sched->list_of(name)); \
        if (!list_empty(&sched->list_of(name)))               \
        {                                                     \
            e = list_pop_front(&sched->list_of(name));        \
            th = apth_t_list_entry(e);                        \
        }                                                     \
        th->belongs_to_list = NULL;                           \
        return th;                                            \
    }                                                         \
    apth_t head_apth_of(name)(apth_sched_t sched)             \
    {                                                         \
        apth_t th = NULL;                                     \
        struct list_elem *e;                                  \
        if (!list_empty(&sched->list_of(name)))               \
        {                                                     \
            e = list_front(&sched->list_of(name));            \
            th = apth_t_list_entry(e);                        \
        }                                                     \
        return th;                                            \
    }

DEFINE_SCHED_LIST_OP(new)
DEFINE_SCHED_LIST_OP(ready)
DEFINE_SCHED_LIST_OP(waiting)
DEFINE_SCHED_LIST_OP(suspended)
DEFINE_SCHED_LIST_OP(terminated)

#undef DEFINE_SCHED_LIST_OP
#undef list_of
#undef new_apth_to
#undef head_apth_of
#undef pop_apth_from
#undef push_apth_to

static bool apth_sched_is_opening(apth_sched_t sched)
{
    return atomic_load_relaxed(&sched->opening);
}

static apth_time_t apth_loadtickgap = APTH_TIME(1, 0);

static void apth_sched_calc_load(apth_sched_t sched, apth_time_t *now)
{
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
}

// Kill the schduler ingredients
static void apth_scheduler_kill(apth_sched_t sched)
{
// Drop all apths
#define CLEAR_T_LIST(name)                     \
    FOR_ELEMENT_IN_LIST(sched->name##_list, e) \
    {                                          \
        apth_t t = apth_t_list_entry(e);       \
        apth_tch_free(t);                      \
    }                                          \
    list_init(&sched->name##_list);

    // Clear the apth queues
    CLEAR_T_LIST(new);
    CLEAR_T_LIST(ready);
    CLEAR_T_LIST(waiting);
    CLEAR_T_LIST(suspended);
    CLEAR_T_LIST(terminated);
#undef CLEAR_T_LIST
    return;

    // Remove the internal signal pipe
    close(sched->apth_sigpipe[0]);
    close(sched->apth_sigpipe[1]);

    // Mark the scheduler as closed
    atomic_store_release(&sched->opening, false);
    return;
}

static bool apth_is_not_null_and_valid(apth_t th)
{
    // Assert sanity
    apth_sched_t sched = cur_sched();
    struct list *sl;
    switch (th->state)
    {
    case APTH_STATE_NEW:
        sl = &sched->new_list;
        break;
    case APTH_STATE_READY:
        sl = &sched->ready_list;
        break;
    case APTH_STATE_WAITING:
        sl = &sched->waiting_list;
        break;
    case APTH_STATE_TERMINATED:
        sl = &sched->terminated_list;
        break;
    default:
        PANIC("should not reach here");
        break;
    }
    struct list *l = th->belongs_to_list;

    assert(l == sl);

    // List contains
    bool found = false;
    FOR_ELEMENT_IN_LIST_REF(l, e)
    {
        apth_t t = apth_t_list_entry(e);
        if (t == th)
            found = true;
    }
    return found;
}

// Start routine for a scheduler pthread. The prototype of this function matches
// that required by Pthread.
static void *scheduler_routine(void *arg)
{
    // Now we are in a separated Pthread
    apth_worker_arg_t worker_arg = (apth_worker_arg_t)arg;
    apth_worker_t me = worker_arg->self;

    // Allocate and initialize the scheduler
    apth_sched_t sched;
    if ((sched = (apth_sched_t)malloc(sizeof(struct apth_perpthr_scheduler))) == NULL)
        return apth_error(NULL, ENOMEM);
    if (!apth_scheduler_init(sched, me))
    {
        // Fail to initialize
        return apth_error(NULL, errno);
    }

    // Set TLS
    set_cur_worker(me);
    set_cur_sched(sched);

    apth_debug("apth_scheduler: bootstrapping");
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
    // TODO: go into a endless loop.
    while (apth_sched_is_opening(sched))
    {
        // TODO: acquire list lock (maybe stealing lock)

        // Move all new threads to ready list
        apth_t th;
        while ((th = pop_apth_from_new(sched)) != APTH_NULL)
        {
            th->state = APTH_STATE_READY;
            // TODO: insert into ready queue according to policy
            // TODO: here just append
            push_apth_to_ready(th, sched);
        }

        // Update statistics
        apth_sched_calc_load(sched, &snapshot);

        // Find the next thread in ready queue and set it to be run
        th = pop_apth_from_ready(sched);

        if (th == APTH_NULL)
        {
            // there is no more thread to ready, panic
            PANIC("APTH SCHEDULER INTERNAL ERROR: no more threads available to schedule");
        }
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
                    pthread_kill(pthread_self(), sig);
                }
            }
        }

        // Set running start time for new thread and perform a context switch to it
        apth_debug("apth scheduler: switching to thread %p (\"%s\")", th, th->name);

        // Update thread times
        apth_time_set(&th->lastran, APTH_TIME_NOW);

        // Update scheduler times
        apth_time_set(&running, &th->lastran);
        apth_time_sub(&running, &snapshot);
        apth_time_add(&sched->running, &running);

        // Switch the thread
        th->dispatches += 1;
        apth_ctx_switch(sched->sched_ctx, th->ctx);

        // Update scheduler times
        apth_time_set(&snapshot, APTH_TIME_NOW);
        apth_debug("apth scheduler: cameback from thread %p (\"%s\")", th, th->name);

        // Update thread times
        apth_time_set(&running, &snapshot);
        apth_time_sub(&running, &th->lastran);
        apth_time_add(&th->running, &running);
        apth_debug("apth_scheduler: thread \"%s\" ran %.6f", th->name, apth_time_t2d(&running));

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
                apth_debug("apth scheduler: stack overflow detected for thread %p (\"%s\")",
                           th, th->name);
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
                pthread_kill(pthread_self(), SIGSEGV);
            }
        }

        // If previous thread is now marked as dead, kick it out
        if (th->state == APTH_STATE_TERMINATED)
        {
            apth_debug("apth scheduler: marking thread \"%s\" as terminated", th->name);
            if (IS_DETACHED(th))
                apth_tcb_free(th);
            else
                push_apth_to_terminated(th, sched);
            set_cur_apth(APTH_NULL);
        }

        // If thread wants to wait for an event, move it to waiting queue now
        if (th != NULL && th->state == APTH_STATE_WAITING)
        {
            apth_debug("apth scheduler: moving thread \"%s\", to waiting queue", th->name);
            push_apth_to_waiting(th, sched);
            set_cur_apth(APTH_NULL);
        }

        // Insert thread back to ready queue if not inserted into waiting queue
        // TODO: migrate old threads in ready queue into higher prio?
        if (th != NULL)
            push_apth_to_ready(th, sched);

        // Manage events in the waiting queue
        if (list_empty(&sched->ready_list) && list_empty(&sched->new_list))
        {
            apth_debug("apth scheduler: no NEW or READY threads, have to wait for new work");
            apth_sched_eventmanager(sched, &snapshot, false /* dopoll */);
        }
        else
        {
            apth_debug("apth scheduler: already NEW or READY threads exists, so just poll for even more work");
            apth_sched_eventmanager(sched, &snapshot, true /* dopoll */);
        }
    }

    // Not reached
    return NULL;
}
