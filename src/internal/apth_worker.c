#include "internal_types.h"
#include "internal_funcs.h"

#include "utils/debug.h"
#include "utils/list.h"
#include "utils/apth_sysutils.h"
#include "utils/atomic_wrapper.h"
#include <string.h>
#include <stdlib.h>

static bool WORKER_POOL_INITIALIZED = false;
struct apth_global_scheduler_pool GLOBAL_POOL;

// TODO: what about add worker ? how to sync that?
_Atomic unsigned int WORKER_SPAWNED = 0x7FFFFFFF;
_Atomic unsigned int SYNC_BEFORE_MAIN_APTH_SPAWN = 0;

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
    assert_msg(result == 0, "fail pthread_setspecific result = %d (%s)", result, strerror(result));
}

APTH_INTERNAL apth_sched_t cur_sched(void)
{
    // return cur_worker()->sched;
    // apth_debug("cur_sched: pthread_getspecific = %p", apth_syscall_raw(pthread_getspecific));
    // return (apth_sched_t)apth_syscall_raw(pthread_getspecific)(__CUR_SCHED_KEY);
    apth_sched_t sched = (apth_sched_t)apth_syscall_raw(pthread_getspecific)(__CUR_SCHED_KEY);
    // apth_debug("cur_sched: got sched = %p", sched);
    return sched;
}

APTH_INTERNAL void set_cur_sched(apth_sched_t sched)
{
    int result = apth_syscall_raw(pthread_setspecific)(__CUR_SCHED_KEY, sched);
    assert_msg(result == 0, "fail pthread_setspecific result = %d (%s)", result, strerror(result));
}

APTH_INTERNAL apth_t cur_apth(void)
{
    return cur_sched()->cur;
}

APTH_INTERNAL void set_cur_apth(apth_t t)
{
    assert(t != APTH_NULL);
    cur_sched()->cur = t;
}

// Get worker by `worker_id`
APTH_INTERNAL apth_worker_t get_worker_by_id(int worker_id)
{
    // Fast path: fortunately worker_id falls in initial worker threads
    // which is usually the situation
    if (0 <= worker_id && worker_id < GLOBAL_POOL.init_worker_count)
    {
        // struct apth_worker_st *p = GLOBAL_POOL.workers_mem_start + worker_id;
        apth_worker_t p = GLOBAL_POOL.worker_ptr_mem_start[worker_id];
        fprintf(stderr, "got p = %p\n", p);
        return p;
    }

    // Slow path: accessing a worker that's added later.
    apth_worker_t result = NULL;

    lll_lock(&GLOBAL_POOL.pool_lock, "get_worker_by_id");
    FOR_ELEMENT_IN_LIST(GLOBAL_POOL.wrkpthrs_list, e)
    {
        apth_worker_t worker = apth_worker_t_list_entry(e);
        if (worker->worker_id == worker_id)
        {
            result = worker;
            break;
        }
    }
    lll_unlock(&GLOBAL_POOL.pool_lock, "get_worker_by_id");
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
    int result;
    worker->worker_id = worker_id;

    // Set detached and CPU affinity
    if ((result = apth_syscall_raw(pthread_attr_init)(&worker->attr)) != 0)
    {
        // apth_debug("fail pthread_attr_init");
        fprintf(stderr, "fail pthread_attr_init\n");
        return apth_error(-1, EINVAL);
    }
    if ((result = apth_syscall_raw(pthread_attr_setdetachstate)(&worker->attr, PTHREAD_CREATE_JOINABLE)) != 0)
    {
        // apth_debug("fail pthread_attr_setdetachstate");
        fprintf(stderr, "fail pthread_attr_setdetachstate\n");
        return apth_error(-1, EINVAL);
    }
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    assert(0 <= worker_id && worker_id < cpu_cores());
    CPU_SET(worker_id, &cpuset);
    if ((result = apth_syscall_raw(pthread_attr_setaffinity_np)(&worker->attr, sizeof(cpu_set_t), &cpuset)) != 0)
    {
        // apth_debug("fail pthread_attr_setaffinity_np");
        fprintf(stderr, "fail pthread_attr_setaffinity_np\n");
        return apth_error(-1, EINVAL);
    }

    // Prepare worker arguments
    apth_worker_arg_t arg;
    if ((arg = (apth_worker_arg_t)malloc(sizeof(struct apth_worker_pthread_arg))) == NULL)
        return apth_error(-1, ENOMEM);
    arg->self = worker;

    // Spawn the worker
    result = apth_syscall_raw(pthread_create)(&worker->tid, &worker->attr, scheduler_routine, arg);
    return result;
}

static int apth_worker_drop(apth_worker_t worker)
{
    // Tell the scheduler to end
    atomic_store_release(&worker->sched->opening, false);

    void *pthr_rslt;
    // TODO: since we spawned all workers as DETACHED, joining here is meaningless.
    apth_syscall_raw(pthread_join)(worker->tid, &pthr_rslt);
    assert(pthr_rslt == NULL);

    free(worker);

    return 0;
}

// Initialize the APTH scheduler pool. The argument indicates whether the caller
// Pthread should also be treated as a worker. For normal situations yes this should
// be true. But for something like JVM, the initializing main thread will continue
// the spawn of JVM in a separated new thread. In such case, since the caller Pthread
// will exit soon, it should not be a worker thread.
APTH_INTERNAL int apth_global_scheduler_pool_init(int init_workers)
{
    if (WORKER_POOL_INITIALIZED)
    {
        PANIC("Worker pool already initialized");
        return -1; // meaningless but make compiler happy
    }

    // long online_cores = cpu_cores();
    int wrkthrs_to_spwan = 0 < init_workers && init_workers <= (int)cpu_cores()
                               ? init_workers
                               : (int)cpu_cores();
    atomic_store_release(&WORKER_SPAWNED, wrkthrs_to_spwan);

    // TODO: initialize the pool lock
    // TODO: acquire pool lock

    // TODO: handle possible OOM with apth_error
    // struct apth_worker_st *workers_mem;
    // if ((workers_mem = malloc(wrkthrs_to_spwan * sizeof(struct apth_worker_st))) == NULL)
    //     return apth_error(-1, ENOMEM);

    apth_worker_t *worker_ptr_mem;
    if ((worker_ptr_mem = (apth_worker_t *)malloc(wrkthrs_to_spwan * sizeof(apth_worker_t))) == NULL)
        return apth_error(-1, ENOMEM);

    list_init(&GLOBAL_POOL.wrkpthrs_list);

    int worker_cnt;
    for (worker_cnt = 0; worker_cnt < wrkthrs_to_spwan; worker_cnt += 1)
    {
        int init_result;
        struct apth_worker_st *workers_mem = malloc(sizeof(struct apth_worker_st));
        if (workers_mem == NULL)
        {
            // TODO: free all previous malloc memory
            return apth_error(-1, ENOMEM);
        }

        if ((init_result = apth_worker_init(workers_mem, worker_cnt)) != 0)
            return apth_error(init_result, errno);
        fprintf(stderr, "spwaned worker %d at %p\n", worker_cnt, workers_mem);
        list_push_back(&GLOBAL_POOL.wrkpthrs_list, &workers_mem->elem);
        // *worker_ptr_mem = workers_mem;
        worker_ptr_mem[worker_cnt] = workers_mem;
    }

    lll_init(&GLOBAL_POOL.pool_lock);
    GLOBAL_POOL.init_worker_count = worker_cnt;
    GLOBAL_POOL.worker_count = wrkthrs_to_spwan;
    GLOBAL_POOL.worker_ptr_mem_start = worker_ptr_mem;
    fprintf(stderr, "Spawned %d workers\n", GLOBAL_POOL.worker_count);
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

    lll_lock(&GLOBAL_POOL.pool_lock, "apth_global_scheduler_pool_drop");
    FOR_ELEMENT_IN_LIST(GLOBAL_POOL.wrkpthrs_list, e)
    {
        apth_worker_t worker = apth_worker_t_list_entry(e);
        int drop_result;
        if ((drop_result = apth_worker_drop(worker)) != 0)
            return apth_error(drop_result, errno);
    }
    lll_unlock(&GLOBAL_POOL.pool_lock, "apth_global_scheduler_pool_drop");

    free(GLOBAL_POOL.worker_ptr_mem_start);
    WORKER_POOL_INITIALIZED = false;

    return 0;
}

APTH_INTERNAL int add_worker_thread(void)
{
    apth_worker_t new_worker;
    if ((new_worker = malloc(sizeof(struct apth_worker_st))) == NULL)
        return apth_error(-1, ENOMEM);

    lll_lock(&GLOBAL_POOL.pool_lock, "add_worker_thread");
    int id = GLOBAL_POOL.worker_count;
    GLOBAL_POOL.worker_count += 1;
    int init_result;
    if ((init_result = apth_worker_init(new_worker, id)) != 0)
        return apth_error(-1, errno);
    list_push_back(&GLOBAL_POOL.wrkpthrs_list, &new_worker->elem);
    lll_unlock(&GLOBAL_POOL.pool_lock, "add_worker_thread");
    return 0;
}
