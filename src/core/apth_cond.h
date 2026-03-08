#ifndef __LIBAPTH_CORE_APTH_COND_H
#define __LIBAPTH_CORE_APTH_COND_H

#include <bits/types/clockid_t.h>
#include "utils/lll_new.h"
#include "utils/list.inline.h"

// Condition variable internal structure
struct apth_cond_st
{
    lll_apth_t guard;    // protects internal fields)
    struct list waiters; // FIFO queue of struct apth_sync_waiter
};

#define APTH_COND_CAST(cond_ptr) ((struct apth_cond_st *)(cond_ptr))
#define APTH_COND_CONST_CAST(cond_ptr) ((const struct apth_cond_st *)(cond_ptr))

// Condition variable attributes internal structure
struct apth_condattr_st
{
    int pshared;        // placeholder
    clockid_t clock_id; // clock for timed waits (CLOCK_REALTIME, CLOCK_MONOTONIC, etc.)
};

#define APTH_CONDATTR_CAST(attr_ptr) ((struct apth_condattr_st *)(attr_ptr))
#define APTH_CONDATTR_CONST_CAST(attr_ptr) ((const struct apth_condattr_st *)(attr_ptr))

#endif // __LIBAPTH_CORE_APTH_COND_H
