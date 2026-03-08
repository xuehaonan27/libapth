#ifndef __LIBAPTH_CORE_APTH_BARRIER_H
#define __LIBAPTH_CORE_APTH_BARRIER_H

#include "utils/lll.h"
#include "utils/list.inline.h"

// Barrier internal structure
struct apth_barrier_st
{
    struct list waiters; // FIFO queue of struct apth_sync_waiter
    lll_apth_t guard;    // protects internal fields)
    uint32_t threshold;  // number of threads that must arrive
    uint32_t count;      // number of threads currently waiting
    uint32_t generation; // increments each time barrier opens
};

#define APTH_BARRIER_CAST(barrier_ptr) ((struct apth_barrier_st *)(barrier_ptr))
#define APTH_BARRIER_CONST_CAST(barrier_ptr) ((const struct apth_barrier_st *)(barrier_ptr))

#endif // __LIBAPTH_CORE_APTH_BARRIER_H
