#define _POSIX_C_SOURCE 199309L
#include <signal.h>
#include "internal_types.h"
#include "internal_funcs.h"

#include "utils/debug.h"
#include "utils/list.h"
#include "utils/apth_sysutils.h"
#include <pthread.h>
#include <string.h>

static bool WORKER_POOL_INITIALIZED = false;
static struct apth_global_scheduler_pool GLOBAL_POOL;

static pthread_key_t __CUR_WORKER_KEY;

void worker_key_t_init(void)
{
    int result = pthread_key_create(&__CUR_WORKER_KEY, worker_key_t_destr_fn);
    assert_msg(result == 0, "fail pthread_key_create");
}

static apth_worker_t cur_worker(void)
{
    return (apth_worker_t)pthread_getspecific(__CUR_WORKER_KEY);
}

static void set_cur_worker(apth_worker_t worker)
{
    int result = pthread_setspecific(__CUR_WORKER_KEY, worker);
    assert_msg(result == 0, "fail pthread_setspecific result = %d", result);
}

static apth_sched_t cur_sched(void)
{
    return cur_worker()->sched;
}

apth_t cur_apth(void)
{
    return cur_sched()->cur;
}

void set_cur_apth(apth_t t)
{
    cur_sched()->cur = t;
}

void worker_key_t_destr_fn(void *p)
{
    // TODO
}

// Get worker by `worker_id`
apth_worker_t get_worker_by_id(int worker_id)
{
    // Fast path: fortunately worker_id falls in initial worker threads
    // which is usually the situation
    if (0 < worker_id && worker_id < GLOBAL_POOL.init_worker_count)
    {
        struct apth_worker_st *p = GLOBAL_POOL.workers_mem_start + worker_id;
        return p;
    }

    // Slow path: accessing a worker that's added later.
    apth_worker_t result = NULL;

    // TODO: lock the GLOBAL_POOL wrkpthrs list lock
    FOR_ELEMENT_IN_LIST(GLOBAL_POOL.wrkpthrs_list, e)
    {
        apth_worker_t worker = apth_worker_t_list_entry(e);
        if (worker->worker_id == worker_id)
        {
            result = worker;
            break;
        }
    }
    // TODO: unlock the lock
    return result;
}

// Count of total workers
int worker_count(void)
{
    int result;
    // TODO: acquire read lock
    // atomic_load(&GLOBAL_POOL.worker_count);
    result = GLOBAL_POOL.worker_count;
    // TODO: release read lock
    return result;
}

// Start routine for a scheduler pthread. The prototype of this function matches
// that required by Pthread.
static void *worker_start_routine(void *arg)
{
    // Now we are in a separated Pthread
    apth_worker_arg_t worker_arg = (apth_worker_arg_t)arg;
    apth_worker_t me = worker_arg->self;

    // Allocate and initialize the scheduler
    apth_sched_t sched;
    if ((sched = (apth_sched_t)malloc(sizeof(struct apth_perpthr_scheduler))) == NULL)
        return apth_error(NULL, ENOMEM);
    apth_scheduler_init(sched, me);

    apth_debug("apth_scheduler: bootstrapping");
    sigset_t sigs;
    apth_time_t snapshot;
    apth_time_t running;
    struct sigaction sa;
    sigset_t ss;

    // block all signals in the scheduler thread
    sigfillset(&sigs);
    apth_syscall(sigprocmask)(SIG_SETMASK, &sigs, NULL);
    // pthread_sigmask(SIG_SETMASK, &sigs, NULL);

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
            if (!th->joinable)
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

static int apth_worker_init(apth_worker_t worker, int worker_id)
{
    worker->worker_id = worker_id;
    pthread_attr_init(&worker->attr);
    apth_worker_arg_t arg;
    if ((arg = (apth_worker_arg_t)malloc(sizeof(struct apth_worker_pthread_arg))) == NULL)
        return apth_error(-1, ENOMEM);
    arg->self = worker;
    // TODO: initialize the worker argument, like core affinity
    // TODO: should this pthread be detached or something?
    int result = pthread_create(&worker->tid, &worker->attr, worker_start_routine, arg);
    return result;
}

// Initialize the APTH scheduler pool. The argument indicates whether the caller
// Pthread should also be treated as a worker. For normal situations yes this should
// be true. But for something like JVM, the initializing main thread will continue
// the spawn of JVM in a separated new thread. In such case, since the caller Pthread
// will exit soon, it should not be a worker thread.
int apth_global_scheduler_pool_init(bool caller_pthr_gets_involved)
{
    if (WORKER_POOL_INITIALIZED)
    {
        PANIC("Worker pool already initialized");
        return -1; // meaningless but make compiler happy
    }

    long online_cores = cpu_cores();

    // TODO: initialize the pool lock

    // TODO: handle possible OOM with apth_error
    struct apth_worker_st *workers_mem;
    if ((workers_mem = malloc(online_cores, sizeof(struct apth_worker_st))) == NULL)
        return apth_error(-1, ENOMEM);

    GLOBAL_POOL.workers_mem_start = workers_mem;
    list_init(&GLOBAL_POOL.wrkpthrs_list);

    int worker_cnt;
    for (worker_cnt = 0; worker_cnt < online_cores;
         worker_cnt += 1, workers_mem += 1)
    {
        int init_result;
        if ((init_result = apth_worker_init(workers_mem, worker_cnt)) != 0)
            return apth_error(init_result, errno);
        list_push_back(&GLOBAL_POOL.wrkpthrs_list, &workers_mem->elem);
    }

    // TODO: if this thread is a worker, then handle it.
    GLOBAL_POOL.init_worker_count = worker_cnt;
    // atomic_store(&GLOBAL_POOL.worker_count, online_cores);
    GLOBAL_POOL.worker_count = online_cores;
    // apth_debug("Spawned %ld workers", atomic_load(&GLOBAL_POOL.worker_count));
    apth_debug("Spawned %ld workers", GLOBAL_POOL.worker_count);
    WORKER_POOL_INITIALIZED = true;

    return 0;
}

int add_worker_thread(void)
{
    apth_worker_t new_worker;
    struct apth_worker_t_list_elem *new_worker_elem;
    if ((new_worker = malloc(sizeof(struct apth_worker_st))) == NULL)
        return apth_error(-1, ENOMEM);

    // TODO: acquire GLOBAL_POOL worker list lock
    int id = GLOBAL_POOL.worker_count;
    GLOBAL_POOL.worker_count += 1;
    int init_result;
    if ((init_result = apth_worker_init(new_worker, id)) != 0)
        return apth_error(-1, errno);
    list_push_back(&GLOBAL_POOL.wrkpthrs_list, &new_worker->elem);
    // TODO: release lock
    return 0;
}
