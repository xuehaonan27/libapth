#ifndef __LIBAPTH_CORE_APTH_RWLOCK_H
#define __LIBAPTH_CORE_APTH_RWLOCK_H

#include "utils/lll_new.h"
#include "utils/list.inline.h"

// Read-write lock internal structure
struct apth_rwlock_st
{
    lll_apth_t guard;       // NEW: Type 1 LLL (protects internal fields)
    int readers;            // count of active readers
    int writers;            // count of active writers (0 or 1)
    int waiting_writers;    // count of writers waiting
    struct list rd_waiters; // FIFO queue of reader struct apth_sync_waiter
    struct list wr_waiters; // FIFO queue of writer struct apth_sync_waiter
};

#define APTH_RWLOCK_CAST(rwlock_ptr) ((struct apth_rwlock_st *)(rwlock_ptr))
#define APTH_RWLOCK_CONST_CAST(rwlock_ptr) ((const struct apth_rwlock_st *)(rwlock_ptr))

#endif // __LIBAPTH_CORE_APTH_RWLOCK_H
