#ifndef __LIBAPTH_INTERNAL_APTH_WORKER_H
#define __LIBAPTH_INTERNAL_APTH_WORKER_H

#include "apth.h"
#include "internal/forward_declare.h"
#include "internal/types/struct_apth_worker_st.h"
#include "utils/archplattoold.h"

// Argument passed to worker's pthread
struct apth_worker_pthread_arg
{
    apth_worker_t self; // pointer to this worker
};
typedef struct apth_worker_pthread_arg *apth_worker_arg_t;

// APTH_INTERNAL void worker_key_t_init(void);
// APTH_INTERNAL void worker_key_t_drop(void);
APTH_INTERNAL apth_worker_t get_worker_by_id(int worker_id);
APTH_INTERNAL int worker_count(void);
APTH_INTERNAL int apth_global_scheduler_pool_init(int init_workers);
APTH_INTERNAL int apth_global_scheduler_pool_drop(void);
APTH_INTERNAL int add_worker_thread(void);
APTH_INTERNAL bool is_main_worker(apth_worker_t worker);

#endif // __LIBAPTH_INTERNAL_APTH_WORKER_H
