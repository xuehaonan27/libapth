#ifndef __LIBAPTH_UTILS_RUNQUEUE_H
#define __LIBAPTH_UTILS_RUNQUEUE_H

#include "utils/archplattoold.h"
#include "utils/lll.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

// Forward declarations
struct apth_st;
typedef struct apth_st *apth_t;

// Runqueue types
typedef enum
{
    APTH_RQ_TYPE_SIMPLE_LIST,   // Simple FIFO queue (for new/waiting/terminated)
    APTH_RQ_TYPE_PRIORITY_HEAP, // Priority heap (for ready queue)
    APTH_RQ_TYPE_MLFQ,          // Multi-Level Feedback Queue (future)
    APTH_RQ_TYPE_CFS_RBTREE     // CFS-style red-black tree (future)
} apth_rq_type_t;

// Forward declaration of runqueue
typedef struct apth_runqueue_st apth_runqueue_t;

// Runqueue operations vtable
typedef struct apth_rq_ops
{
    // Basic operations
    int (*enqueue)(apth_runqueue_t *rq, apth_t th);
    apth_t (*dequeue)(apth_runqueue_t *rq);
    apth_t (*peek)(apth_runqueue_t *rq);
    bool (*remove)(apth_runqueue_t *rq, apth_t th);

    // Priority-aware operations
    apth_t (*pick_next)(apth_runqueue_t *rq); // Select next thread to run
    bool (*requeue_with_priority)(apth_runqueue_t *rq, apth_t th, int new_prio);

    // Work stealing support
    apth_t (*steal_task)(apth_runqueue_t *rq); // Steal a task from this queue

    // Query operations
    bool (*contains)(apth_runqueue_t *rq, apth_t th);
    size_t (*size)(apth_runqueue_t *rq);
    bool (*empty)(apth_runqueue_t *rq);

    // Lifecycle
    void (*destroy)(apth_runqueue_t *rq);
} apth_rq_ops_t;

// Unified runqueue structure
struct apth_runqueue_st
{
    apth_rq_type_t type; // Queue type
    void *impl;          // Pointer to concrete implementation

    // Synchronization
    lll_t lock;                // Queue lock
    _Atomic bool being_stolen; // Whether being stolen from

    // Statistics
    _Atomic size_t count; // Number of elements in queue

    // Operations vtable
    const apth_rq_ops_t *ops;
};

// Initialization functions for different queue types
APTH_INTERNAL int apth_rq_init_simple_list(apth_runqueue_t *rq);
APTH_INTERNAL int apth_rq_init_priority_heap(apth_runqueue_t *rq, size_t initial_capacity);

// Generic runqueue operations (delegate to vtable)
APTH_INTERNAL int apth_rq_enqueue(apth_runqueue_t *rq, apth_t th);
APTH_INTERNAL apth_t apth_rq_dequeue(apth_runqueue_t *rq);
APTH_INTERNAL apth_t apth_rq_peek(apth_runqueue_t *rq);
APTH_INTERNAL bool apth_rq_remove(apth_runqueue_t *rq, apth_t th);
APTH_INTERNAL apth_t apth_rq_pick_next(apth_runqueue_t *rq);
APTH_INTERNAL bool apth_rq_requeue_with_priority(apth_runqueue_t *rq, apth_t th, int new_prio);
APTH_INTERNAL apth_t apth_rq_steal_task(apth_runqueue_t *rq);
APTH_INTERNAL bool apth_rq_contains(apth_runqueue_t *rq, apth_t th);
APTH_INTERNAL size_t apth_rq_size(apth_runqueue_t *rq);
APTH_INTERNAL bool apth_rq_empty(apth_runqueue_t *rq);
APTH_INTERNAL void apth_rq_destroy(apth_runqueue_t *rq);

// Thread-safe locked operations
APTH_INTERNAL int apth_rq_enqueue_locked(apth_runqueue_t *rq, apth_t th);
APTH_INTERNAL apth_t apth_rq_dequeue_locked(apth_runqueue_t *rq);
APTH_INTERNAL bool apth_rq_remove_locked(apth_runqueue_t *rq, apth_t th);
APTH_INTERNAL apth_t apth_rq_pick_next_locked(apth_runqueue_t *rq);

// Lock operations
APTH_INTERNAL void apth_rq_lock(apth_runqueue_t *rq, const char *from);
APTH_INTERNAL void apth_rq_unlock(apth_runqueue_t *rq, const char *from);
APTH_INTERNAL bool apth_rq_trylock(apth_runqueue_t *rq);

// Iterator support - for compatibility with code that needs to traverse queues
// Callback function type for iteration
typedef void (*apth_rq_iter_func_t)(apth_t th, void *arg);

// Iterate over all threads in queue (must hold lock)
APTH_INTERNAL void apth_rq_foreach(apth_runqueue_t *rq, apth_rq_iter_func_t func, void *arg);

// Get underlying list for simple list queues (for backward compatibility)
// Returns NULL for non-list queues
APTH_INTERNAL struct list *apth_rq_get_list(apth_runqueue_t *rq);

#endif // __LIBAPTH_UTILS_RUNQUEUE_H
