#ifndef __LIBAPTH_CORE_APTH_SEM_H
#define __LIBAPTH_CORE_APTH_SEM_H

#include "utils/lll_new.h"
#include "utils/list.inline.h"

// Semaphore internal structure
struct apth_sem_st
{
    lll_apth_t guard;    // protects internal fields)
    unsigned int value;  // current count
    struct list waiters; // FIFO queue of struct apth_sync_waiter
};

#define APTH_SEM_CAST(sem_ptr) ((struct apth_sem_st *)(sem_ptr))
#define APTH_SEM_CONST_CAST(sem_ptr) ((const struct apth_sem_st *)(sem_ptr))

#endif // __LIBAPTH_CORE_APTH_SEM_H
