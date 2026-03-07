#ifndef __LIBAPTH_INTERNAL_APTH_THQUEUE_H
#define __LIBAPTH_INTERNAL_APTH_THQUEUE_H

#include "internal/forward_declare.h"
#include "internal/apth_tcb.h"
#include "internal/apth_state.h"
#include "utils/lll_new.h"
#include "utils/list.inline.h"
#include "utils/archplattoold.h"
#include "utils/atomic_wrapper.h"

struct apth_thqueue_st
{
    struct list th_list;
    lll_internal_t th_list_lock; // NEW: Type 2 LLL (replaces lll_t)
    apth_sched_t sched;
    apth_state_t th_state; // State that apths belonging to this queue should be
    size_t size;           // NEW: Non-atomic, protected by lock
};

typedef void drain_thqueue_th_func(apth_t th);
// This return values from `visit_thqueue_th_func` indicates not an apth_thqueue,
// but just to tell `visit_thqueue`, take that count into its return value, but
// do not move `th`. Useful in fist loop of event manager.
#define APTH_DONT_MOVE_BUT_COUNT (apth_thqueue_t)(-1)
typedef apth_thqueue_t visit_thqueue_th_func(apth_t th, void *);
typedef bool find_first_in_thqueue_th_func(apth_t th, void *);

APTH_INTERNAL void thqueue_init(apth_thqueue_t *queue, apth_sched_t sched, apth_state_t state);
APTH_INTERNAL void push_apth_to(apth_thqueue_t queue, apth_t th);
APTH_INTERNAL apth_t pop_apth_from(apth_thqueue_t queue);
APTH_INTERNAL void remove_apth_from(apth_thqueue_t queue, apth_t th);
APTH_INTERNAL void drain_thqueue(apth_thqueue_t queue, drain_thqueue_th_func fn);
APTH_INTERNAL void transfer_th(apth_t th, apth_thqueue_t from, apth_thqueue_t to);
APTH_INTERNAL apth_t transfer_one_th(apth_thqueue_t from, apth_thqueue_t to,
                                     bool insert_from_front, const char *dbg_msg);
APTH_INTERNAL size_t visit_thqueue(apth_thqueue_t queue, visit_thqueue_th_func fn, void *);
APTH_INTERNAL apth_t find_first_in_thqueue(apth_thqueue_t queue, find_first_in_thqueue_th_func fn, void *aux);


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

#endif // __LIBAPTH_INTERNAL_APTH_THQUEUE_H
