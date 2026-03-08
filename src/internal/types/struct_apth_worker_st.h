#ifndef __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_WORKER_ST_H
#define __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_WORKER_ST_H

#include "internal/types/struct_apth_sched_st.h"
#include <pthread.h>

// Pthread worker occupying CPU and carrying APTH loads
struct apth_worker_st
{
    int worker_id;       // worker ID
    pthread_t tid;       // a worker pthread
    pthread_attr_t attr; // Worker pthread attribute
    apth_sched_t sched;  // Hold scheduler
    struct list_elem elem;
#define apth_worker_t_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_worker_st, elem)
};

#endif // __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_WORKER_ST_H
