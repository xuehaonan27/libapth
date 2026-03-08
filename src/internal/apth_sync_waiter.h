#ifndef __LIBAPTH_INTERNAL_APTH_SYNC_WAITER_H
#define __LIBAPTH_INTERNAL_APTH_SYNC_WAITER_H

#include "apth.h"
#include "internal/types.h"
#include "utils/list.h"

// Waiter node for sync primitives (stack-allocated on the waiting thread).
// The thread's stack frame is frozen while it is in WAITING state, so
// this struct persists until the thread is woken.
struct apth_sync_waiter
{
    apth_t th;               // the blocked apth thread
    struct apth_event_st ev; // inline event (no separate allocation)
    struct list_elem elem;   // link into primitive's waiter queue
#define apth_sync_waiter_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_sync_waiter, elem)
};

#endif // __LIBAPTH_INTERNAL_APTH_SYNC_WAITER_H
