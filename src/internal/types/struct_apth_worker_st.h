#ifndef __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_WORKER_ST_H
#define __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_WORKER_ST_H

#include "internal/types/struct_apth_sched_st.h"
#include <pthread.h>

/* Startup state published by scheduler_routine so the init/cleanup path
 * knows whether the worker came up successfully or not. */
enum apth_worker_state
{
    APTH_WORKER_STARTING = 0, /* calloc default — pthread spawned, not yet ready */
    APTH_WORKER_READY,        /* scheduler_routine fully initialized */
    APTH_WORKER_FAILED,       /* scheduler_routine hit early-return error */
};

// Pthread worker occupying CPU and carrying APTH loads
struct apth_worker_st
{
    int worker_id;       // worker ID
    pthread_t tid;       // a worker pthread
    pthread_attr_t attr; // Worker pthread attribute
    apth_sched_t sched;  // Hold scheduler
    _Atomic(enum apth_worker_state) state; // Startup lifecycle
    struct list_elem elem;
#define apth_worker_t_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_worker_st, elem)
};

#endif // __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_WORKER_ST_H
