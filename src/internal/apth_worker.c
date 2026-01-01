#include "internal_types.h"

#include "utils/debug.h"
#include "utils/list.h"
#include "utils/apth_sysutils.h"
#include <pthread.h>
#include <string.h>

static bool WORKER_POOL_INITIALIZED = false;
static struct apth_global_scheduler_pool GLOBAL_POOL;

_Thread_local apth_worker_t __CUR_WORKER;
_Thread_local apth_sched_t __CUR_SCHED;

apth_worker_t cur_worker(void)
{
    return __CUR_WORKER;
}

apth_sched_t cur_sched(void)
{
    return __CUR_SCHED;
}

apth_t cur_apth(void)
{
    return cur_sched()->running;
}

// Get worker by `worker_id`
apth_worker_t get_worker(int worker_id)
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
    struct list_elem *e;
    struct list *glb_worker_list = &GLOBAL_POOL.wrkpthrs_list;
    for (e = list_begin(glb_worker_list);
         e != list_end(glb_worker_list);
         e = list_next(e))
    {
        struct apth_worker_t_list_elem *elem = apth_worker_t_list_entry(e);
        apth_worker_t worker = elem->pworker;
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
    // now we are in a separated Pthread
    apth_worker_arg_t worker_arg = (apth_worker_arg_t)arg;
    apth_worker_t me = worker_arg->self;

    // allocate and initialize the scheduler
    apth_sched_t sched;
    if ((sched = (apth_sched_t)malloc(sizeof(struct apth_perpthr_scheduler))) == NULL)
        return apth_error(NULL, ENOMEM);
    int sched_init_result = apth_scheduler_init(sched, me);

    // TODO: initialize other parts of the worker
    // TODO: go into a endless loop.
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

void apth_worker_elem_init(struct apth_worker_t_list_elem *elem, apth_worker_t worker)
{
    memset(elem, 0, sizeof(struct apth_worker_t_list_elem));
    elem->pworker = worker;
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
    struct apth_worker_t_list_elem *worker_elems_mem;
    if ((worker_elems_mem = malloc(online_cores, sizeof(struct apth_worker_t_list_elem))) == NULL)
        return apth_error(-1, ENOMEM);

    GLOBAL_POOL.workers_mem_start = workers_mem;
    GLOBAL_POOL.worker_elems_mem_start = worker_elems_mem;
    list_init(&GLOBAL_POOL.wrkpthrs_list);

    int worker_cnt;
    for (worker_cnt = 0; worker_cnt < online_cores;
         worker_cnt += 1, workers_mem += 1, worker_elems_mem += 1)
    {
        int init_result;
        if ((init_result = apth_worker_init(workers_mem, worker_cnt)) != 0)
            return apth_error(init_result, errno);
        apth_worker_elem_init(worker_elems_mem, workers_mem);
        list_push_back(&GLOBAL_POOL.wrkpthrs_list, &worker_elems_mem->elem);
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
    if ((new_worker_elem = malloc(sizeof(struct apth_worker_t_list_elem))) == NULL)
        return apth_error(-1, ENOMEM);

    // TODO: acquire GLOBAL_POOL worker list lock
    int id = GLOBAL_POOL.worker_count;
    GLOBAL_POOL.worker_count += 1;
    int init_result;
    if ((init_result = apth_worker_init(new_worker, id)) != 0)
        return apth_error(-1, errno);
    apth_worker_elem_init(new_worker_elem, new_worker);
    list_push_back(&GLOBAL_POOL.wrkpthrs_list, &new_worker_elem->elem);
    // TODO: release lock
    return 0;
}
