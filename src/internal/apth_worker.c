#include "internal_types.h"
#include "internal_funcs.h"

#include "utils/debug.h"
#include "utils/list.h"
#include "utils/apth_sysutils.h"
#include <string.h>
#include <stdlib.h>

static bool WORKER_POOL_INITIALIZED = false;
static struct apth_global_scheduler_pool GLOBAL_POOL;

static pthread_key_t __CUR_WORKER_KEY;
static pthread_key_t __CUR_SCHED_KEY;

static void worker_key_t_destr_fn(void *) { /* nop */ }
static void sched_key_t_destr_fn(void *) { /* nop */ }

void worker_key_t_init(void)
{
    int result = apth_syscall_raw(pthread_key_create)(&__CUR_WORKER_KEY, worker_key_t_destr_fn);
    assert_msg(result == 0, "fail pthread_key_create");
}

void sched_key_t_init(void)
{
    int result = apth_syscall_raw(pthread_key_create)(&__CUR_SCHED_KEY, sched_key_t_destr_fn);
    assert_msg(result == 0, "fail pthread_key_create");
}

APTH_INTERNAL apth_worker_t cur_worker(void)
{
    return (apth_worker_t)apth_syscall_raw(pthread_getspecific)(__CUR_WORKER_KEY);
}

APTH_INTERNAL void set_cur_worker(apth_worker_t worker)
{
    int result = apth_syscall_raw(pthread_setspecific)(__CUR_WORKER_KEY, worker);
    assert_msg(result == 0, "fail pthread_setspecific result = %d", result);
}

APTH_INTERNAL apth_sched_t cur_sched(void)
{
    // return cur_worker()->sched;
    return (apth_sched_t)apth_syscall_raw(pthread_getspecific)(__CUR_SCHED_KEY);
}

APTH_INTERNAL void set_cur_sched(apth_sched_t sched)
{
    int result = apth_syscall_raw(pthread_setspecific)(__CUR_WORKER_KEY, sched);
    assert_msg(result == 0, "fail pthread_setspecific result = %d", result);
}

APTH_INTERNAL apth_t cur_apth(void)
{
    return cur_sched()->cur;
}

APTH_INTERNAL void set_cur_apth(apth_t t)
{
    cur_sched()->cur = t;
}

// Get worker by `worker_id`
APTH_INTERNAL apth_worker_t get_worker_by_id(int worker_id)
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
APTH_INTERNAL int worker_count(void)
{
    int result;
    // TODO: acquire read lock
    // atomic_load(&GLOBAL_POOL.worker_count);
    result = GLOBAL_POOL.worker_count;
    // TODO: release read lock
    return result;
}

static int apth_worker_init(apth_worker_t worker, int worker_id)
{
    worker->worker_id = worker_id;
    apth_syscall_raw(pthread_attr_init)(&worker->attr);
    apth_worker_arg_t arg;
    if ((arg = (apth_worker_arg_t)malloc(sizeof(struct apth_worker_pthread_arg))) == NULL)
        return apth_error(-1, ENOMEM);
    arg->self = worker;
    // TODO: initialize the worker argument, like core affinity
    // TODO: should this pthread be detached or something?
    int result = apth_syscall_raw(pthread_create)(&worker->tid, &worker->attr, scheduler_routine, arg);
    return result;
}

static int apth_worker_drop(apth_worker_t worker)
{
    // Drop the scheduler
    apth_scheduler_kill(worker->sched);

    void *pthr_rslt;
    apth_syscall_raw(pthread_join)(worker->tid, &pthr_rslt);
    assert(pthr_rslt == NULL);
    return 0;
}

// Initialize the APTH scheduler pool. The argument indicates whether the caller
// Pthread should also be treated as a worker. For normal situations yes this should
// be true. But for something like JVM, the initializing main thread will continue
// the spawn of JVM in a separated new thread. In such case, since the caller Pthread
// will exit soon, it should not be a worker thread.
APTH_INTERNAL int apth_global_scheduler_pool_init(void)
{
    if (WORKER_POOL_INITIALIZED)
    {
        PANIC("Worker pool already initialized");
        return -1; // meaningless but make compiler happy
    }

    long online_cores = cpu_cores();

    // TODO: initialize the pool lock
    // TODO: acquire pool lock

    // TODO: handle possible OOM with apth_error
    struct apth_worker_st *workers_mem;
    if ((workers_mem = malloc(online_cores * sizeof(struct apth_worker_st))) == NULL)
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

APTH_INTERNAL int apth_global_scheduler_pool_drop(void)
{
    if (!WORKER_POOL_INITIALIZED)
    {
        PANIC("Worker pool not initialized!");
        return -1;
    }

    // TODO: acquire pool lock

    FOR_ELEMENT_IN_LIST(GLOBAL_POOL.wrkpthrs_list, e)
    {
        apth_worker_t worker = apth_worker_t_list_entry(e);
        int drop_result;
        if ((drop_result = apth_worker_drop(worker)) != 0)
            return apth_error(drop_result, errno);
    }

    free(GLOBAL_POOL.workers_mem_start);
    WORKER_POOL_INITIALIZED = false;

    return 0;
}

APTH_INTERNAL int add_worker_thread(void)
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
