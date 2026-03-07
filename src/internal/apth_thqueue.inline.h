#ifndef __LIBAPTH_INTERNAL_APTH_THQUEUE_INLINE_H
#define __LIBAPTH_INTERNAL_APTH_THQUEUE_INLINE_H

#include "internal_types.h"
#include "utils/archplattoold.h"
#include "internal/apth_worker.inline.h"

INLINE apth_thqueue_t belonging_queue_of(apth_t th, const char *dbg_msg)
{
    apth_thqueue_t q = atomic_load_acquire(&th->belongs_to_queue);
    assert_msg(q != NULL, "calling belonging_queue_of from: %s", dbg_msg);
    return q;
}

INLINE void set_belonging_queue_of(apth_t th, apth_thqueue_t q)
{
    atomic_store_release(&th->belongs_to_queue, q);
}

INLINE size_t thqueue_size(apth_thqueue_t queue)
{
    assert(queue != NULL);
    // NEW: Size is no longer atomic, but reading without lock is acceptable
    // for approximate size checks (e.g., work stealing heuristics)
    return queue->size;
}

INLINE bool apth_is_in(apth_thqueue_t queue, apth_t th)
{
    assert(queue != NULL);
    return (belonging_queue_of(th, "apth_is_in") == queue);
}

#endif // __LIBAPTH_INTERNAL_APTH_THQUEUE_INLINE_H
