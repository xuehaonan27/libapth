#include "apth_worker.h"
#include "hook_libc/hooked_funcs.h"
#include "internal/apth_sched.h"
#include "internal/apth_global_sched_pool.h"
#include "utils/debug.h"
#include "utils/list.inline.h"
#include "utils/apth_sysutils.h"
#include "utils/atomic_wrapper.h"
#include "utils/lll.inline.h"
#include <string.h>
#include <stdlib.h>

static bool WORKER_POOL_INITIALIZED = false;
struct apth_global_scheduler_pool GLOBAL_POOL;

// TODO: what about add worker ? how to sync that?
_Atomic(unsigned int) WORKER_SPAWNED = 0x7FFFFFFF;
_Atomic(unsigned int) SYNC_BEFORE_MAIN_APTH_SPAWN = 0;

// Get worker by `worker_id`
APTH_INTERNAL apth_worker_t get_worker_by_id(int worker_id)
{
    // Fast path: fortunately worker_id falls in initial worker threads
    // which is usually the situation
    if (0 <= worker_id && worker_id < GLOBAL_POOL.init_worker_count)
    {
        apth_worker_t p = GLOBAL_POOL.worker_ptr_mem_start[worker_id];
        return p;
    }

    // Slow path: accessing a worker that's added later.
    apth_worker_t result = NULL;

    lll_internal_lock(&GLOBAL_POOL.pool_lock);
    FOR_ELEMENT_IN_LIST(GLOBAL_POOL.wrkpthrs_list, e)
    {
        apth_worker_t worker = apth_worker_t_list_entry(e);
        if (worker->worker_id == worker_id)
        {
            result = worker;
            break;
        }
    }
    lll_internal_unlock(&GLOBAL_POOL.pool_lock);
    return result;
}

// Count of total workers
APTH_INTERNAL int worker_count(void)
{
    int result;
    lll_internal_lock(&GLOBAL_POOL.pool_lock);
    result = GLOBAL_POOL.worker_count;
    lll_internal_unlock(&GLOBAL_POOL.pool_lock);
    return result;
}

APTH_INTERNAL bool is_main_worker(apth_worker_t worker)
{
    return worker->worker_id == 0;
}

static int apth_worker_init(apth_worker_t worker, int worker_id)
{
    apth_debug("enter");

    int result;
    worker->worker_id = worker_id;

    // Set detached and CPU affinity
    if ((result = apth_func_raw(pthread_attr_init)(&worker->attr)) != 0)
    {
        apth_debug("fail pthread_attr_init");
        return apth_error(-1, EINVAL);
    }

    int spawn_detach_state;
    if (is_main_worker(worker))
#ifdef APTH_HOLD_INITIALIZER_PTHREAD
        spawn_detach_state = PTHREAD_CREATE_JOINABLE;
#else
        spawn_detach_state = PTHREAD_CREATE_DETACHED;
#endif // APTH_HOLD_INITIALIZER_PTHREAD
    else
        spawn_detach_state = PTHREAD_CREATE_JOINABLE;

    if ((result = apth_func_raw(pthread_attr_setdetachstate)(&worker->attr, spawn_detach_state)) != 0)
    {
        apth_debug("fail pthread_attr_setdetachstate");
        return apth_error(-1, EINVAL);
    }
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    assert(0 <= worker_id && worker_id < cpu_cores());
    int actual_cpu = apth_worker_id_to_cpu(worker_id);
    CPU_SET(actual_cpu, &cpuset);
    if ((result = apth_func_raw(pthread_attr_setaffinity_np)(&worker->attr, sizeof(cpu_set_t), &cpuset)) != 0)
    {
        apth_debug("fail pthread_attr_setaffinity_np");
        return apth_error(-1, EINVAL);
    }

    // Prepare worker arguments (freed by scheduler_routine after extraction)
    apth_worker_arg_t arg;
    if ((arg = (apth_worker_arg_t)malloc(sizeof(struct apth_worker_pthread_arg))) == NULL)
        return apth_error(-1, ENOMEM);
    arg->self = worker;

    // Spawn the worker
    result = apth_func_raw(pthread_create)(&worker->tid, &worker->attr, scheduler_routine, arg);

    apth_debug("leave");
    return result;
}

static int apth_worker_drop(apth_worker_t worker)
{
    if (worker == NULL)
        return 0;
    int target_worker_id = worker->worker_id;
    apth_debug("enter, dropping worker %d sched = %p", target_worker_id, worker->sched);
    if (worker->sched == NULL)
    {
        free(worker);
        return 0;
    }
    // Tell the scheduler to clean and exit
    atomic_store_release(&worker->sched->opening, false);

    // Wake the scheduler in case it's blocked in epoll_wait
    apth_sched_wake(worker->sched);

    void *pthr_rslt;
    apth_func_raw(pthread_join)(worker->tid, &pthr_rslt);
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
    apth_debug("enter");

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

    apth_worker_t *worker_ptr_mem;
    if ((worker_ptr_mem = (apth_worker_t *)malloc(wrkthrs_to_spwan * sizeof(apth_worker_t))) == NULL)
        return apth_error(-1, ENOMEM);

    list_init(&GLOBAL_POOL.wrkpthrs_list);

    int worker_cnt;
    for (worker_cnt = 0; worker_cnt < wrkthrs_to_spwan; worker_cnt += 1)
    {
        int init_result;
        struct apth_worker_st *workers_mem = calloc(1, sizeof(struct apth_worker_st));
        if (workers_mem == NULL)
        {
            goto init_fail_cleanup;
        }

        if ((init_result = apth_worker_init(workers_mem, worker_cnt)) != 0)
        {
            free(workers_mem);
            goto init_fail_cleanup;
        }
        apth_debug("spwaned worker %d at %p", worker_cnt, workers_mem);
        list_push_back(&GLOBAL_POOL.wrkpthrs_list, &workers_mem->elem);
        worker_ptr_mem[worker_cnt] = workers_mem;
    }

    lll_internal_init(&GLOBAL_POOL.pool_lock);
    GLOBAL_POOL.init_worker_count = worker_cnt;
    GLOBAL_POOL.worker_count = wrkthrs_to_spwan;
    GLOBAL_POOL.worker_ptr_mem_start = worker_ptr_mem;
    apth_debug("Spawned %d workers", GLOBAL_POOL.worker_count);
    WORKER_POOL_INITIALIZED = true;

    apth_debug("leave");

    return 0;

init_fail_cleanup:
    /* Shut down workers that were already started.
     * Wait for each worker to reach READY or FAILED before cleanup.
     * READY workers need apth_worker_drop (sets opening=false, joins).
     * FAILED workers need only pthread_join + free (no sched exists). */
    for (int i = 0; i < worker_cnt; i++)
    {
        apth_worker_t w = worker_ptr_mem[i];
        if (w == NULL)
            continue;
        /* Wait for scheduler_routine to publish its outcome. */
        while (atomic_load_acquire(&w->state) == APTH_WORKER_STARTING)
            sched_yield();
        if (atomic_load_acquire(&w->state) == APTH_WORKER_READY)
            apth_worker_drop(w);
        else
        {
            /* FAILED: scheduler_routine already returned; just join + free. */
            void *pthr_rslt;
            apth_func_raw(pthread_join)(w->tid, &pthr_rslt);
            free(w);
        }
    }
    free(worker_ptr_mem);
    return apth_error(-1, ENOMEM);
}

APTH_INTERNAL int apth_global_scheduler_pool_drop(void)
{
    if (!WORKER_POOL_INITIALIZED)
    {
        // Lazy-start: pool was never started (all DEDICATED threads).
        // Nothing to drop — return success.
        return 0;
    }

    lll_internal_lock(&GLOBAL_POOL.pool_lock);
    // FOR_ELEMENT_IN_LIST(GLOBAL_POOL.wrkpthrs_list, e)
    {
        struct list_elem *e;
        // for (e = list_begin(&GLOBAL_POOL.wrkpthrs_list);
        // e != list_end(&GLOBAL_POOL.wrkpthrs_list);
        // e = list_next(e))
        while (!list_empty(&GLOBAL_POOL.wrkpthrs_list))
        {
            e = list_pop_back(&GLOBAL_POOL.wrkpthrs_list);

            apth_worker_t worker = apth_worker_t_list_entry(e);
            int drop_result;
            if ((drop_result = apth_worker_drop(worker)) != 0)
                return apth_error(drop_result, errno);
        }
    }
    lll_internal_unlock(&GLOBAL_POOL.pool_lock);

    free(GLOBAL_POOL.worker_ptr_mem_start);
    WORKER_POOL_INITIALIZED = false;

    return 0;
}

APTH_INTERNAL int add_worker_thread(void)
{
    apth_worker_t new_worker;
    if ((new_worker = malloc(sizeof(struct apth_worker_st))) == NULL)
        return apth_error(-1, ENOMEM);

    lll_internal_lock(&GLOBAL_POOL.pool_lock);
    int id = GLOBAL_POOL.worker_count;
    GLOBAL_POOL.worker_count += 1;
    int init_result;
    if ((init_result = apth_worker_init(new_worker, id)) != 0)
        return apth_error(-1, errno);
    list_push_back(&GLOBAL_POOL.wrkpthrs_list, &new_worker->elem);
    lll_internal_unlock(&GLOBAL_POOL.pool_lock);
    return 0;
}
