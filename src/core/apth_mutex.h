#ifndef __LIBAPTH_CORE_APTH_MUTEX_H
#define __LIBAPTH_CORE_APTH_MUTEX_H

#include "utils/lll_new.h"
#include "utils/list.inline.h"

// Mutex internal structure
struct apth_mutex_st
{
    lll_apth_t guard;        // NEW: Type 1 LLL (protects internal fields)
    apth_t owner;            // current lock owner, or NULL
    unsigned int lock_count; // recursion depth for RECURSIVE type
    int type;                // APTH_MUTEX_NORMAL, _ERRORCHECK, _RECURSIVE
    struct list waiters;     // FIFO queue of struct apth_sync_waiter
};

// Helper macros to cast between opaque union and internal struct
#define APTH_MUTEX_CAST(mutex_ptr) ((struct apth_mutex_st *)(mutex_ptr))
#define APTH_MUTEX_CONST_CAST(mutex_ptr) ((const struct apth_mutex_st *)(mutex_ptr))

// Mutex attributes internal structure
struct apth_mutexattr_st
{
    int type;
};

#define APTH_MUTEXATTR_CAST(attr_ptr) ((struct apth_mutexattr_st *)(attr_ptr))
#define APTH_MUTEXATTR_CONST_CAST(attr_ptr) ((const struct apth_mutexattr_st *)(attr_ptr))

#endif // __LIBAPTH_CORE_APTH_MUTEX_H
