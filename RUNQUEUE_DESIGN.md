# Runqueue Design and Implementation

## Overview

This document describes the new unified runqueue abstraction layer designed to replace the current `struct list + lll_t` approach in libapth. The new design addresses several critical issues:

1. **Atomicity**: State transitions and queue operations are now atomic
2. **Priority Support**: Built-in priority scheduling with O(log n) heap operations
3. **Thread Safety**: Fine-grained locking with support for work stealing
4. **Extensibility**: Plugin architecture supports multiple scheduling algorithms

## Architecture

### Core Components

#### 1. `apth_runqueue_t` - Unified Queue Interface
```c
struct apth_runqueue_st {
    apth_rq_type_t type;          // Queue implementation type
    void *impl;                   // Concrete implementation
    lll_t lock;                   // Queue lock
    _Atomic bool being_stolen;    // Work stealing flag
    _Atomic size_t count;         // Element count
    const apth_rq_ops_t *ops;     // Virtual function table
};
```

#### 2. Virtual Function Table (VTable)
```c
struct apth_rq_ops {
    int (*enqueue)(apth_runqueue_t *rq, apth_t th);
    apth_t (*dequeue)(apth_runqueue_t *rq);
    apth_t (*peek)(apth_runqueue_t *rq);
    bool (*remove)(apth_runqueue_t *rq, apth_t th);
    apth_t (*pick_next)(apth_runqueue_t *rq);
    bool (*requeue_with_priority)(apth_runqueue_t *rq, apth_t th, int new_prio);
    apth_t (*steal_task)(apth_runqueue_t *rq);
    bool (*contains)(apth_runqueue_t *rq, apth_t target);
    size_t (*size)(apth_runqueue_t *rq);
    bool (*empty)(apth_runqueue_t *rq);
    void (*destroy)(apth_runqueue_t *rq);
};
```

## Implementations

### 1. Simple List (APTH_RQ_TYPE_SIMPLE_LIST)

**Use Case**: For new, waiting, and terminated queues where FIFO order is sufficient.

**Characteristics**:
- O(1) enqueue/dequeue
- O(n) contains/remove
- Supports work stealing from back

**Initialization**:
```c
apth_runqueue_t queue;
apth_rq_init_simple_list(&queue);
```

### 2. Priority Heap (APTH_RQ_TYPE_PRIORITY_HEAP)

**Use Case**: For ready queue where threads should be scheduled by priority.

**Characteristics**:
- O(log n) enqueue
- O(1) peek highest priority
- O(log n) dequeue
- O(n) remove (requires search)
- O(log n) priority updates with re-heapify
- Min-heap: smaller `prio` value = higher priority

**Initialization**:
```c
apth_runqueue_t ready_queue;
apth_rq_init_priority_heap(&ready_queue, 32);  // Initial capacity 32
```

## Atomic State Transitions

### Design

The `apth_atomic_state_transition()` function ensures that state changes and queue operations happen atomically:

1. Acquire necessary locks (in consistent order to prevent deadlock)
2. Verify current state (if required)
3. Remove from source queue
4. Update thread state
5. Add to destination queue
6. Release locks

### Example Usage

```c
// Transition from NEW to READY
apth_state_transition_t trans = {
    .thread = th,
    .from_state = APTH_STATE_NEW,
    .to_state = APTH_STATE_READY,
    .from_queue = &sched->new_queue,
    .to_queue = &sched->ready_queue,
    .cross_scheduler = false,
    .skip_state_check = false
};
int result = apth_atomic_state_transition(&trans);
```

### Helper Functions

For convenience, common transitions have helper functions:

```c
// NEW -> READY
apth_transition_new_to_ready(th, &sched->new_queue, &sched->ready_queue);

// READY -> (Running, dequeued)
apth_transition_ready_to_running(th, &sched->ready_queue);

// WAITING -> READY  
apth_transition_waiting_to_ready(th, &sched->waiting_queue, &sched->ready_queue);

// Any state -> TERMINATED
apth_transition_to_terminated(th, NULL, &sched->terminated_queue);
```

## Migration Guide

### Phase 1: Add New Queue Support (DONE)

Files created:
- `src/utils/apth_runqueue.h` - Queue interface
- `src/utils/apth_runqueue.c` - Implementation
- `src/utils/apth_state_transition.h` - State transition interface  
- `src/utils/apth_state_transition.c` - Implementation

### Phase 2: Update Scheduler Structure (TODO)

Modify `struct apth_perpthr_scheduler` in `src/internal_types.h`:

```c
// OLD:
struct list new_list;
struct list ready_list;
struct list waiting_list;
struct list terminated_list;
lll_t new_list_lock;
lll_t ready_list_lock;
lll_t waiting_list_lock;
lll_t terminated_list_lock;

// NEW:
apth_runqueue_t new_queue;        // Simple list
apth_runqueue_t ready_queue;      // Priority heap
apth_runqueue_t waiting_queue;    // Simple list
apth_runqueue_t terminated_queue; // Simple list
```

### Phase 3: Update Scheduler Initialization (TODO)

In `apth_scheduler_init()`:

```c
// OLD:
list_init(&sched->new_list);
list_init(&sched->ready_list);
list_init(&sched->waiting_list);
list_init(&sched->terminated_list);
lll_init(&sched->new_list_lock);
lll_init(&sched->ready_list_lock);
lll_init(&sched->waiting_list_lock);
lll_init(&sched->terminated_list_lock);

// NEW:
apth_rq_init_simple_list(&sched->new_queue);
apth_rq_init_priority_heap(&sched->ready_queue, 0);  // Auto-size
apth_rq_init_simple_list(&sched->waiting_queue);
apth_rq_init_simple_list(&sched->terminated_queue);
```

### Phase 4: Update Queue Operations (TODO)

Replace all `push_apth_to_*`, `pop_apth_from_*`, etc. calls:

```c
// OLD:
push_apth_to_ready(th, sched);
th = pop_apth_from_ready(sched);
remove_apth(th);

// NEW:
apth_rq_enqueue_locked(&sched->ready_queue, th);
th = apth_rq_dequeue_locked(&sched->ready_queue);
apth_rq_remove_locked(th->belongs_to_list_lock, th);
```

### Phase 5: Fix Race Conditions (TODO)

Update `apth_join.c` and `apth_cancel.c`:

```c
// OLD (racy):
if (tid->state != APTH_STATE_TERMINATED) {
    apth_wait_event(ev);
}
wait_apth_to_be_in_list(tid);
remove_apth(tid);

// NEW (atomic):
if (tid->state != APTH_STATE_TERMINATED) {
    apth_wait_event(ev);
}
apth_wait_for_queue_placement(tid);  // Better than busy-wait
apth_rq_remove_locked(tid->belongs_to_list_lock, tid);
```

## Benefits

### 1. Solves Race Conditions

**Before**: In `apth_join.c`, thread could be TERMINATED but not yet in terminated_list
**After**: State transition and queue placement are atomic

### 2. Priority Scheduling

**Before**: O(n) to find highest priority thread in list
**After**: O(1) to get highest priority thread from heap

### 3. Better Work Stealing

**Before**: Need manual locking logic
**After**: Built-in `steal_task()` with proper synchronization

### 4. Extensibility

Easy to add new scheduling algorithms:
- Multi-Level Feedback Queue (MLFQ)
- CFS-style Red-Black Tree
- Earliest Deadline First (EDF)

## Performance Characteristics

| Operation | Simple List | Priority Heap |
|-----------|-------------|---------------|
| Enqueue | O(1) | O(log n) |
| Dequeue | O(1) | O(log n) |
| Peek | O(1) | O(1) |
| Pick Next | O(1) | O(1) |
| Remove | O(n) | O(n) |
| Contains | O(n) | O(n) |
| Requeue w/ Priority | O(1)* | O(log n) |

*Simple list doesn't reorder, just updates value

## Future Enhancements

### 1. Work Stealing Implementation

```c
apth_t apth_steal_work(apth_sched_t victim, apth_sched_t thief) {
    apth_runqueue_t *rq = &victim->ready_queue;
    
    // Non-blocking attempt to steal
    if (!apth_rq_trylock(rq)) {
        return NULL;
    }
    
    atomic_store(&rq->being_stolen, true);
    apth_t stolen = apth_rq_steal_task(rq);
    
    if (stolen != NULL) {
        stolen->worker = thief->worker;
    }
    
    atomic_store(&rq->being_stolen, false);
    apth_rq_unlock(rq, "steal_work");
    
    return stolen;
}
```

### 2. Priority Inheritance Protocol

```c
void apth_handle_priority_inversion(apth_t low_prio_holder, apth_t high_prio_waiter) {
    if (low_prio_holder->prio > high_prio_waiter->prio) {
        // Save original priority
        int original_prio = low_prio_holder->prio;
        
        // Boost priority
        apth_runqueue_t *rq = (apth_runqueue_t *)low_prio_holder->belongs_to_list;
        if (rq != NULL) {
            apth_rq_lock(rq, "priority_inheritance");
            apth_rq_requeue_with_priority(rq, low_prio_holder, high_prio_waiter->prio);
            apth_rq_unlock(rq, "priority_inheritance");
        }
    }
}
```

### 3. CFS-Style Scheduler

```c
// Red-black tree based on virtual runtime
struct apth_cfs_runqueue {
    struct apth_rbtree_node *root;
    struct apth_rbtree_node *leftmost;  // Cached minimum
    uint64_t min_vruntime;
};

// O(1) pick next task (cached leftmost)
// O(log n) enqueue/dequeue
```

## Testing

### Unit Tests Needed

1. Test priority heap correctness
   - Enqueue in random order, dequeue should be sorted
   - Test heap property after remove
   - Test priority updates

2. Test atomic state transitions
   - Verify no intermediate states are visible
   - Test cross-scheduler transitions
   - Test error handling and rollback

3. Test work stealing
   - Steal from back doesn't interfere with dequeue from front
   - Proper lock handling

4. Stress test race conditions
   - Multiple threads joining same target
   - Cancellation during state transition
   - Work stealing during normal operation

## Files Modified/Created

### Created
- `src/utils/apth_runqueue.h`
- `src/utils/apth_runqueue.c`
- `src/utils/apth_state_transition.h`
- `src/utils/apth_state_transition.c`
- `RUNQUEUE_DESIGN.md` (this file)

### To Be Modified
- `src/internal_types.h` - Update scheduler struct
- `src/internal/apth_sched.c` - Use new queue API
- `src/core/apth_join.c` - Use atomic transitions
- `src/internal/apth_cancel.c` - Use atomic transitions
- `src/core/apth_create.c` - Update queue initialization
- `Makefile` - Already auto-detects new .c files

## Conclusion

The new runqueue abstraction provides:
1. **Correctness**: Atomic state transitions eliminate race conditions
2. **Performance**: Priority scheduling with O(1) pick_next
3. **Scalability**: Fine-grained locking ready for work stealing
4. **Maintainability**: Clean abstraction makes adding new schedulers easy

The design follows best practices from Linux kernel's scheduler and provides a solid foundation for future enhancements.
