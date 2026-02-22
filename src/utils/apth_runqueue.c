#include "apth_runqueue.h"
#include "list.h"
#include "debug.h"
#include "atomic_wrapper.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

// Include internal_types.h to get full apth_t definition
#include "../internal_types.h"

// ============================================================================
// Simple List Implementation
// ============================================================================

typedef struct
{
    struct list list;
} apth_rq_simple_list_t;

// Simple list operations
static int simple_list_enqueue(apth_runqueue_t *rq, apth_t th)
{
    apth_rq_simple_list_t *impl = (apth_rq_simple_list_t *)rq->impl;
    assert(th != NULL);
    assert(APTH_IS_VALID(th));

    list_push_back(&impl->list, &th->elem);
    atomic_fetch_add_explicit(&rq->count, 1, memory_order_release);

    // Update thread's belonging information
    th->belongs_to_list = (struct list *)&impl->list;
    th->belongs_to_list_lock = &rq->lock;

    return 0;
}

static apth_t simple_list_dequeue(apth_runqueue_t *rq)
{
    apth_rq_simple_list_t *impl = (apth_rq_simple_list_t *)rq->impl;

    if (list_empty(&impl->list))
    {
        return NULL;
    }

    struct list_elem *e = list_pop_front(&impl->list);
    apth_t th = apth_t_list_entry(e);
    atomic_fetch_sub_explicit(&rq->count, 1, memory_order_release);

    // Clear thread's belonging information
    th->belongs_to_list = NULL;
    th->belongs_to_list_lock = NULL;

    return th;
}

static apth_t simple_list_peek(apth_runqueue_t *rq)
{
    apth_rq_simple_list_t *impl = (apth_rq_simple_list_t *)rq->impl;

    if (list_empty(&impl->list))
    {
        return NULL;
    }

    struct list_elem *e = list_front(&impl->list);
    return apth_t_list_entry(e);
}

static bool simple_list_remove(apth_runqueue_t *rq, apth_t th)
{
    apth_rq_simple_list_t *impl = (apth_rq_simple_list_t *)rq->impl;
    assert(th != NULL);

    // Verify the thread belongs to this queue
    if (th->belongs_to_list != (struct list *)&impl->list)
    {
        return false;
    }

    list_remove(&th->elem);
    atomic_fetch_sub_explicit(&rq->count, 1, memory_order_release);

    // Clear thread's belonging information
    th->belongs_to_list = NULL;
    th->belongs_to_list_lock = NULL;

    return true;
}

static apth_t simple_list_pick_next(apth_runqueue_t *rq)
{
    // For simple list, pick_next is same as peek
    return simple_list_peek(rq);
}

static bool simple_list_requeue_with_priority(apth_runqueue_t *rq, apth_t th, int new_prio)
{
    // Simple list doesn't support priority reordering
    // Just update the priority value
    th->prio = new_prio;
    return true;
}

static apth_t simple_list_steal_task(apth_runqueue_t *rq)
{
    apth_rq_simple_list_t *impl = (apth_rq_simple_list_t *)rq->impl;

    // Steal from the back to reduce contention with dequeue (from front)
    if (list_empty(&impl->list))
    {
        return NULL;
    }

    struct list_elem *e = list_pop_back(&impl->list);
    apth_t th = apth_t_list_entry(e);
    atomic_fetch_sub_explicit(&rq->count, 1, memory_order_release);

    // Clear thread's belonging information
    th->belongs_to_list = NULL;
    th->belongs_to_list_lock = NULL;

    return th;
}

static bool simple_list_contains(apth_runqueue_t *rq, apth_t target)
{
    apth_rq_simple_list_t *impl = (apth_rq_simple_list_t *)rq->impl;

    FOR_ELEMENT_IN_LIST_REF(&impl->list, e)
    {
        apth_t th = apth_t_list_entry(e);
        if (th == target)
        {
            return true;
        }
    }
    return false;
}

static size_t simple_list_size(apth_runqueue_t *rq)
{
    return atomic_load_explicit(&rq->count, memory_order_acquire);
}

static bool simple_list_empty(apth_runqueue_t *rq)
{
    return simple_list_size(rq) == 0;
}

static void simple_list_destroy(apth_runqueue_t *rq)
{
    if (rq->impl != NULL)
    {
        free(rq->impl);
        rq->impl = NULL;
    }
}

static const apth_rq_ops_t simple_list_ops = {
    .enqueue = simple_list_enqueue,
    .dequeue = simple_list_dequeue,
    .peek = simple_list_peek,
    .remove = simple_list_remove,
    .pick_next = simple_list_pick_next,
    .requeue_with_priority = simple_list_requeue_with_priority,
    .steal_task = simple_list_steal_task,
    .contains = simple_list_contains,
    .size = simple_list_size,
    .empty = simple_list_empty,
    .destroy = simple_list_destroy,
};

// ============================================================================
// Priority Heap Implementation
// ============================================================================

#define APTH_HEAP_INITIAL_CAPACITY 32

typedef struct
{
    apth_t *heap; // Array of thread pointers
    size_t capacity;
    size_t size;
    // Min-heap: smaller prio value = higher priority
} apth_rq_priority_heap_t;

// Helper functions for heap
static inline size_t heap_parent(size_t i) { return (i - 1) / 2; }
static inline size_t heap_left(size_t i) { return 2 * i + 1; }
static inline size_t heap_right(size_t i) { return 2 * i + 2; }

static void heap_swap(apth_t *heap, size_t i, size_t j)
{
    apth_t temp = heap[i];
    heap[i] = heap[j];
    heap[j] = temp;
}

static void heap_sift_up(apth_t *heap, size_t idx)
{
    while (idx > 0)
    {
        size_t parent = heap_parent(idx);
        // Min-heap: parent should have smaller or equal priority value
        if (heap[parent]->prio <= heap[idx]->prio)
        {
            break;
        }
        heap_swap(heap, idx, parent);
        idx = parent;
    }
}

static void heap_sift_down(apth_t *heap, size_t size, size_t idx)
{
    while (true)
    {
        size_t smallest = idx;
        size_t left = heap_left(idx);
        size_t right = heap_right(idx);

        if (left < size && heap[left]->prio < heap[smallest]->prio)
        {
            smallest = left;
        }
        if (right < size && heap[right]->prio < heap[smallest]->prio)
        {
            smallest = right;
        }

        if (smallest == idx)
        {
            break;
        }

        heap_swap(heap, idx, smallest);
        idx = smallest;
    }
}

static bool heap_resize(apth_rq_priority_heap_t *impl, size_t new_capacity)
{
    apth_t *new_heap = (apth_t *)realloc(impl->heap, new_capacity * sizeof(apth_t));
    if (new_heap == NULL)
    {
        return false;
    }
    impl->heap = new_heap;
    impl->capacity = new_capacity;
    return true;
}

// Priority heap operations
static int priority_heap_enqueue(apth_runqueue_t *rq, apth_t th)
{
    apth_rq_priority_heap_t *impl = (apth_rq_priority_heap_t *)rq->impl;
    assert(th != NULL);
    assert(APTH_IS_VALID(th));

    // Resize if needed
    if (impl->size >= impl->capacity)
    {
        if (!heap_resize(impl, impl->capacity * 2))
        {
            return -ENOMEM;
        }
    }

    // Add to end and sift up
    impl->heap[impl->size] = th;
    heap_sift_up(impl->heap, impl->size);
    impl->size++;

    atomic_fetch_add_explicit(&rq->count, 1, memory_order_release);

    // Note: For heap, we don't use belongs_to_list since position changes
    // Instead we'll search when needed
    th->belongs_to_list = (struct list *)impl; // Use as marker
    th->belongs_to_list_lock = &rq->lock;

    return 0;
}

static apth_t priority_heap_dequeue(apth_runqueue_t *rq)
{
    apth_rq_priority_heap_t *impl = (apth_rq_priority_heap_t *)rq->impl;

    if (impl->size == 0)
    {
        return NULL;
    }

    // Remove root (highest priority)
    apth_t th = impl->heap[0];
    impl->size--;

    if (impl->size > 0)
    {
        impl->heap[0] = impl->heap[impl->size];
        heap_sift_down(impl->heap, impl->size, 0);
    }

    atomic_fetch_sub_explicit(&rq->count, 1, memory_order_release);

    // Clear belonging information
    th->belongs_to_list = NULL;
    th->belongs_to_list_lock = NULL;

    return th;
}

static apth_t priority_heap_peek(apth_runqueue_t *rq)
{
    apth_rq_priority_heap_t *impl = (apth_rq_priority_heap_t *)rq->impl;

    if (impl->size == 0)
    {
        return NULL;
    }

    return impl->heap[0];
}

static bool priority_heap_remove(apth_runqueue_t *rq, apth_t target)
{
    apth_rq_priority_heap_t *impl = (apth_rq_priority_heap_t *)rq->impl;

    // Find the thread in heap (O(n) operation)
    size_t idx;
    bool found = false;
    for (idx = 0; idx < impl->size; idx++)
    {
        if (impl->heap[idx] == target)
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        return false;
    }

    // Remove by replacing with last element and re-heapify
    impl->size--;
    if (idx < impl->size)
    {
        impl->heap[idx] = impl->heap[impl->size];

        // Try both sift up and sift down
        heap_sift_up(impl->heap, idx);
        heap_sift_down(impl->heap, impl->size, idx);
    }

    atomic_fetch_sub_explicit(&rq->count, 1, memory_order_release);

    target->belongs_to_list = NULL;
    target->belongs_to_list_lock = NULL;

    return true;
}

static apth_t priority_heap_pick_next(apth_runqueue_t *rq)
{
    // For priority heap, pick_next is same as peek (highest priority)
    return priority_heap_peek(rq);
}

static bool priority_heap_requeue_with_priority(apth_runqueue_t *rq, apth_t th, int new_prio)
{
    apth_rq_priority_heap_t *impl = (apth_rq_priority_heap_t *)rq->impl;

    // Find the thread
    size_t idx;
    bool found = false;
    for (idx = 0; idx < impl->size; idx++)
    {
        if (impl->heap[idx] == th)
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        return false;
    }

    int old_prio = th->prio;
    th->prio = new_prio;

    // Re-heapify based on priority change
    if (new_prio < old_prio)
    {
        // Priority increased, sift up
        heap_sift_up(impl->heap, idx);
    }
    else if (new_prio > old_prio)
    {
        // Priority decreased, sift down
        heap_sift_down(impl->heap, impl->size, idx);
    }

    return true;
}

static apth_t priority_heap_steal_task(apth_runqueue_t *rq)
{
    apth_rq_priority_heap_t *impl = (apth_rq_priority_heap_t *)rq->impl;

    // Steal from the back (lowest priority) to reduce contention
    if (impl->size == 0)
    {
        return NULL;
    }

    impl->size--;
    apth_t th = impl->heap[impl->size];

    atomic_fetch_sub_explicit(&rq->count, 1, memory_order_release);

    th->belongs_to_list = NULL;
    th->belongs_to_list_lock = NULL;

    return th;
}

static bool priority_heap_contains(apth_runqueue_t *rq, apth_t target)
{
    apth_rq_priority_heap_t *impl = (apth_rq_priority_heap_t *)rq->impl;

    for (size_t i = 0; i < impl->size; i++)
    {
        if (impl->heap[i] == target)
        {
            return true;
        }
    }
    return false;
}

static size_t priority_heap_size(apth_runqueue_t *rq)
{
    return atomic_load_explicit(&rq->count, memory_order_acquire);
}

static bool priority_heap_empty(apth_runqueue_t *rq)
{
    return priority_heap_size(rq) == 0;
}

static void priority_heap_destroy(apth_runqueue_t *rq)
{
    if (rq->impl != NULL)
    {
        apth_rq_priority_heap_t *impl = (apth_rq_priority_heap_t *)rq->impl;
        if (impl->heap != NULL)
        {
            free(impl->heap);
        }
        free(impl);
        rq->impl = NULL;
    }
}

static const apth_rq_ops_t priority_heap_ops = {
    .enqueue = priority_heap_enqueue,
    .dequeue = priority_heap_dequeue,
    .peek = priority_heap_peek,
    .remove = priority_heap_remove,
    .pick_next = priority_heap_pick_next,
    .requeue_with_priority = priority_heap_requeue_with_priority,
    .steal_task = priority_heap_steal_task,
    .contains = priority_heap_contains,
    .size = priority_heap_size,
    .empty = priority_heap_empty,
    .destroy = priority_heap_destroy,
};

// ============================================================================
// Initialization Functions
// ============================================================================

APTH_INTERNAL int apth_rq_init_simple_list(apth_runqueue_t *rq)
{
    apth_rq_simple_list_t *impl = (apth_rq_simple_list_t *)malloc(sizeof(apth_rq_simple_list_t));
    if (impl == NULL)
    {
        return -ENOMEM;
    }

    list_init(&impl->list);

    rq->type = APTH_RQ_TYPE_SIMPLE_LIST;
    rq->impl = impl;
    lll_init(&rq->lock);
    atomic_store_explicit(&rq->being_stolen, false, memory_order_release);
    atomic_store_explicit(&rq->count, 0, memory_order_release);
    rq->ops = &simple_list_ops;

    return 0;
}

APTH_INTERNAL int apth_rq_init_priority_heap(apth_runqueue_t *rq, size_t initial_capacity)
{
    apth_rq_priority_heap_t *impl = (apth_rq_priority_heap_t *)malloc(sizeof(apth_rq_priority_heap_t));
    if (impl == NULL)
    {
        return -ENOMEM;
    }

    if (initial_capacity == 0)
    {
        initial_capacity = APTH_HEAP_INITIAL_CAPACITY;
    }

    impl->heap = (apth_t *)malloc(initial_capacity * sizeof(apth_t));
    if (impl->heap == NULL)
    {
        free(impl);
        return -ENOMEM;
    }

    impl->capacity = initial_capacity;
    impl->size = 0;

    rq->type = APTH_RQ_TYPE_PRIORITY_HEAP;
    rq->impl = impl;
    lll_init(&rq->lock);
    atomic_store_explicit(&rq->being_stolen, false, memory_order_release);
    atomic_store_explicit(&rq->count, 0, memory_order_release);
    rq->ops = &priority_heap_ops;

    return 0;
}

// ============================================================================
// Generic Runqueue Operations (delegate to vtable)
// ============================================================================

APTH_INTERNAL int apth_rq_enqueue(apth_runqueue_t *rq, apth_t th)
{
    assert(rq != NULL && rq->ops != NULL && rq->ops->enqueue != NULL);
    return rq->ops->enqueue(rq, th);
}

APTH_INTERNAL apth_t apth_rq_dequeue(apth_runqueue_t *rq)
{
    assert(rq != NULL && rq->ops != NULL && rq->ops->dequeue != NULL);
    return rq->ops->dequeue(rq);
}

APTH_INTERNAL apth_t apth_rq_peek(apth_runqueue_t *rq)
{
    assert(rq != NULL && rq->ops != NULL && rq->ops->peek != NULL);
    return rq->ops->peek(rq);
}

APTH_INTERNAL bool apth_rq_remove(apth_runqueue_t *rq, apth_t th)
{
    assert(rq != NULL && rq->ops != NULL && rq->ops->remove != NULL);
    return rq->ops->remove(rq, th);
}

APTH_INTERNAL apth_t apth_rq_pick_next(apth_runqueue_t *rq)
{
    assert(rq != NULL && rq->ops != NULL && rq->ops->pick_next != NULL);
    return rq->ops->pick_next(rq);
}

APTH_INTERNAL bool apth_rq_requeue_with_priority(apth_runqueue_t *rq, apth_t th, int new_prio)
{
    assert(rq != NULL && rq->ops != NULL && rq->ops->requeue_with_priority != NULL);
    return rq->ops->requeue_with_priority(rq, th, new_prio);
}

APTH_INTERNAL apth_t apth_rq_steal_task(apth_runqueue_t *rq)
{
    assert(rq != NULL && rq->ops != NULL && rq->ops->steal_task != NULL);
    return rq->ops->steal_task(rq);
}

APTH_INTERNAL bool apth_rq_contains(apth_runqueue_t *rq, apth_t target)
{
    assert(rq != NULL && rq->ops != NULL && rq->ops->contains != NULL);
    return rq->ops->contains(rq, target);
}

APTH_INTERNAL size_t apth_rq_size(apth_runqueue_t *rq)
{
    assert(rq != NULL && rq->ops != NULL && rq->ops->size != NULL);
    return rq->ops->size(rq);
}

APTH_INTERNAL bool apth_rq_empty(apth_runqueue_t *rq)
{
    assert(rq != NULL && rq->ops != NULL && rq->ops->empty != NULL);
    return rq->ops->empty(rq);
}

APTH_INTERNAL void apth_rq_destroy(apth_runqueue_t *rq)
{
    assert(rq != NULL && rq->ops != NULL && rq->ops->destroy != NULL);
    rq->ops->destroy(rq);
}

// ============================================================================
// Thread-safe Locked Operations
// ============================================================================

APTH_INTERNAL int apth_rq_enqueue_locked(apth_runqueue_t *rq, apth_t th)
{
    apth_rq_lock(rq, "apth_rq_enqueue_locked");
    int result = apth_rq_enqueue(rq, th);
    apth_rq_unlock(rq, "apth_rq_enqueue_locked");
    return result;
}

APTH_INTERNAL apth_t apth_rq_dequeue_locked(apth_runqueue_t *rq)
{
    apth_rq_lock(rq, "apth_rq_dequeue_locked");
    apth_t th = apth_rq_dequeue(rq);
    apth_rq_unlock(rq, "apth_rq_dequeue_locked");
    return th;
}

APTH_INTERNAL bool apth_rq_remove_locked(apth_runqueue_t *rq, apth_t th)
{
    apth_rq_lock(rq, "apth_rq_remove_locked");
    bool result = apth_rq_remove(rq, th);
    apth_rq_unlock(rq, "apth_rq_remove_locked");
    return result;
}

APTH_INTERNAL apth_t apth_rq_pick_next_locked(apth_runqueue_t *rq)
{
    apth_rq_lock(rq, "apth_rq_pick_next_locked");
    apth_t th = apth_rq_pick_next(rq);
    apth_rq_unlock(rq, "apth_rq_pick_next_locked");
    return th;
}

// ============================================================================
// Lock Operations
// ============================================================================

APTH_INTERNAL void apth_rq_lock(apth_runqueue_t *rq, const char *from)
{
    lll_lock(&rq->lock, from);
}

APTH_INTERNAL void apth_rq_unlock(apth_runqueue_t *rq, const char *from)
{
    lll_unlock(&rq->lock, from);
}

APTH_INTERNAL bool apth_rq_trylock(apth_runqueue_t *rq)
{
    // Try to acquire lock non-blocking
    // For now, we'll implement a simple version
    // A proper trylock would need to be added to lll.h/lll.c
    // For now, just return false (not implemented)
    (void)rq;
    return false;
}

// ============================================================================
// Iterator Support
// ============================================================================

APTH_INTERNAL void apth_rq_foreach(apth_runqueue_t *rq, apth_rq_iter_func_t func, void *arg)
{
    assert(rq != NULL && func != NULL);

    if (rq->type == APTH_RQ_TYPE_SIMPLE_LIST)
    {
        apth_rq_simple_list_t *impl = (apth_rq_simple_list_t *)rq->impl;
        FOR_ELEMENT_IN_LIST_REF(&impl->list, e)
        {
            apth_t th = apth_t_list_entry(e);
            func(th, arg);
        }
    }
    else if (rq->type == APTH_RQ_TYPE_PRIORITY_HEAP)
    {
        apth_rq_priority_heap_t *impl = (apth_rq_priority_heap_t *)rq->impl;
        for (size_t i = 0; i < impl->size; i++)
        {
            func(impl->heap[i], arg);
        }
    }
}

APTH_INTERNAL struct list *apth_rq_get_list(apth_runqueue_t *rq)
{
    assert(rq != NULL);

    if (rq->type == APTH_RQ_TYPE_SIMPLE_LIST)
    {
        apth_rq_simple_list_t *impl = (apth_rq_simple_list_t *)rq->impl;
        return &impl->list;
    }

    return NULL; // Non-list queue types return NULL
}
