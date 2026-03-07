#ifndef __LIBAPTH_INTERNAL_APTH_GLOBAL_SCHED_POOL_H
#define __LIBAPTH_INTERNAL_APTH_GLOBAL_SCHED_POOL_H

#include "internal/apth_worker.h"
#include "utils/lll_new.h"

// The whole process shares this pool. This pool should be placed in heap.
// Accessing to this pool should be synchronized.
struct apth_global_scheduler_pool
{
    lll_internal_t pool_lock;  // Type 2 LLL - protect access to the pool
    int worker_count;          // total worker pthreads count
    struct list wrkpthrs_list; // worker pthreads [elem: struct apth_worker_t_list_elem]

    /* Immutable fields */
    int init_worker_count;               // initially spawned workers
    apth_worker_t *worker_ptr_mem_start; // start memory address of pointers to init workers
};

extern struct apth_global_scheduler_pool GLOBAL_POOL;

#endif // __LIBAPTH_INTERNAL_APTH_GLOBAL_SCHED_POOL_H
