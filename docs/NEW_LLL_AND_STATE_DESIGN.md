# New LLL and State Management Design

## Overview

This document describes the redesigned Low-Level Lock (LLL) system and simplified state management for LIBAPTH. The new design eliminates the ROBBER mechanism, simplifies state transitions, and provides clearer separation of concerns.

## Motivation

The previous LLL design had several issues:
1. **Complex ROBBER mechanism**: Designed to handle scheduler/APTH deadlocks that never actually occur
2. **Dual state representation**: Both `state_holder` (uncommitted/committed) and `belongs_to_queue` represented state
3. **NULL pointer risks**: `sched_of(th)` required dereferencing `belongs_to_queue->sched`, which could be NULL
4. **Complex queue operations**: Transfer operations required complex locking to avoid NULL dereferences

## New Design Principles

### 1. Two Types of Locks

**Type 1 LLL: Synchronization Primitives (APTH-only)**
- Used for: mutex, cond, sem, rwlock, barrier guards
- Only APTHs acquire these locks
- Stores APTH pointer to identify owner
- Yields on contention
- Provides `trylock` for non-blocking attempts

**Type 2 LLL: Internal LIBAPTH (Mixed)**
- Used for: queue locks, signal locks, pool locks
- Can be acquired by both APTHs and schedulers
- Stores only a flag (0 or 1), no pointer needed
- Behavior depends on caller:
  - APTH: yields on contention
  - Scheduler: spins on contention
- No `trylock` needed

### 2. Simplified State Management

**Old Design:**
```c
volatile _Atomic(apth_state_t) state_holder;  // Uncommitted/committed
_Atomic(apth_thqueue_t) belongs_to_queue;     // Implies state via queue->th_state
```

**New Design:**
```c
_Atomic(apth_state_t) state;                  // Simple atomic state
apth_sched_t home_sched;                      // Immutable: set at creation
apth_sched_t current_sched;                   // Mutable: current owner
apth_thqueue_t current_queue;                 // Protected by queue lock
lll_internal_t ownership_lock;                // For cross-scheduler operations
```

### 3. Key Insights

**Why ROBBER is Not Needed:**
- Type 1 locks are only acquired by APTHs
- Type 2 locks can be acquired by both, but:
  - Schedulers only acquire locks of non-running APTHs
  - No deadlock scenario exists where scheduler needs a lock held by its own APTH

**Why Uncommitted/Committed is Not Needed (Mostly):**
- For most state transitions, simple atomic stores suffice
- For TERMINATED state, we change state while holding the terminated queue lock
- This ensures atomicity of "state change + queue insertion" without complex protocols

## Implementation Details

### Type 1 LLL API

```c
typedef struct {
    _Atomic(apth_t) owner;  // NULL or APTH pointer
} lll_apth_t;

void lll_apth_init(lll_apth_t *lock);
void lll_apth_lock(lll_apth_t *lock);
int  lll_apth_trylock(lll_apth_t *lock);  // Returns 0 on success, EBUSY on failure
void lll_apth_unlock(lll_apth_t *lock);
```

### Type 2 LLL API

```c
typedef struct {
    _Atomic(unsigned char) locked;  // 0 or 1
} lll_internal_t;

void lll_internal_init(lll_internal_t *lock);
void lll_internal_lock(lll_internal_t *lock);
void lll_internal_unlock(lll_internal_t *lock);
```

### State Transitions

**Simple transitions (most cases):**
```c
atomic_store(&th->state, APTH_STATE_READY);
atomic_store(&th->state, APTH_STATE_WAITING);
```

**TERMINATED transition (special case):**
```c
void transition_to_terminated(apth_t th, apth_sched_t sched) {
    lll_internal_lock(&sched->running_queue->lock);
    lll_internal_lock(&sched->terminated_queue->lock);

    // Remove from running queue
    list_remove(&th->elem);
    sched->running_queue->size--;

    // Insert into terminated queue
    list_push_back(&sched->terminated_queue->th_list, &th->elem);
    th->current_queue = sched->terminated_queue;
    sched->terminated_queue->size++;

    // Change state WHILE HOLDING terminated queue lock
    atomic_store(&th->state, APTH_STATE_TERMINATED);

    lll_internal_unlock(&sched->terminated_queue->lock);
    lll_internal_unlock(&sched->running_queue->lock);
}
```

### Queue Operations

**Simple push:**
```c
void push_apth_to(apth_thqueue_t queue, apth_t th) {
    lll_internal_lock(&queue->lock);
    list_push_back(&queue->th_list, &th->elem);
    th->current_queue = queue;
    queue->size++;
    lll_internal_unlock(&queue->lock);
}
```

**Simple transfer:**
```c
void transfer_th(apth_t th, apth_thqueue_t from, apth_thqueue_t to) {
    lll_internal_lock(&from->lock);
    lll_internal_lock(&to->lock);

    list_remove(&th->elem);
    from->size--;

    list_push_back(&to->th_list, &th->elem);
    th->current_queue = to;
    to->size++;

    lll_internal_unlock(&to->lock);
    lll_internal_unlock(&from->lock);
}
```

### Work Stealing

```c
apth_t steal_work(apth_sched_t thief, apth_sched_t victim) {
    lll_internal_lock(&victim->ready_queue->lock);
    // ... check and pop from victim
    lll_internal_lock(&th->ownership_lock);
    lll_internal_unlock(&victim->ready_queue->lock);

    th->current_sched = thief;

    lll_internal_lock(&thief->ready_queue->lock);
    // ... insert into thief
    lll_internal_unlock(&thief->ready_queue->lock);
    lll_internal_unlock(&th->ownership_lock);

    return th;
}
```

### Join Operation

```c
int apth_join(apth_t target, void **retval) {
    // Wait for TERMINATED state
    while (atomic_load(&target->state) != APTH_STATE_TERMINATED) {
        // Wait via event
    }

    lll_internal_lock(&target->ownership_lock);

    apth_thqueue_t term_queue = target->current_sched->terminated_queue;
    lll_internal_lock(&term_queue->lock);

    if (target->current_queue != term_queue) {
        // Already removed
        lll_internal_unlock(&term_queue->lock);
        lll_internal_unlock(&target->ownership_lock);
        return EINVAL;
    }

    list_remove(&target->elem);
    term_queue->size--;
    target->current_queue = NULL;

    lll_internal_unlock(&term_queue->lock);
    lll_internal_unlock(&target->ownership_lock);

    if (retval) *retval = target->join_arg;
    apth_tcb_free(target);
    return 0;
}
```

## Migration Plan

### Phase 1: Add New LLL Types (DONE)
- [x] Create `lll_new.h` and `lll_new.inline.h`
- [x] Implement `lll_apth_t` and `lll_internal_t`
- [x] Add comments to `internal_types.h` showing new fields

### Phase 2: Update Synchronization Primitives
- [ ] Update mutex to use `lll_apth_t`
- [ ] Update cond to use `lll_apth_t`
- [ ] Update sem to use `lll_apth_t`
- [ ] Update rwlock to use `lll_apth_t`
- [ ] Update barrier to use `lll_apth_t`

### Phase 3: Update Internal Locks
- [ ] Update queue locks to use `lll_internal_t`
- [ ] Update signal locks to use `lll_internal_t`
- [ ] Update pool lock to use `lll_internal_t`
- [ ] Update fd close lock to use `lll_internal_t`

### Phase 4: Add Ownership System
- [ ] Add `home_sched`, `current_sched`, `current_queue`, `ownership_lock` to `apth_st`
- [ ] Initialize these fields in `apth_create`
- [ ] Update work stealing to use ownership lock
- [ ] Update join to use ownership lock

### Phase 5: Simplify State Management
- [ ] Add simple `state` field to `apth_st`
- [ ] Update state transitions to use simple atomic stores
- [ ] Special-case TERMINATED transition to hold queue lock
- [ ] Update event manager to rely on TERMINATED atomicity

### Phase 6: Remove Old Code
- [ ] Remove `state_holder` and uncommitted/committed logic
- [ ] Remove `belongs_to_queue` atomic operations
- [ ] Remove old `lll.h` and `lll.inline.h`
- [ ] Remove ROBBER mechanism
- [ ] Remove `sched_of` calls from LLL

### Phase 7: Testing
- [ ] Unit tests for new LLL types
- [ ] Integration tests for state transitions
- [ ] Stress tests for work stealing and join
- [ ] ThreadSanitizer runs
- [ ] Performance benchmarks

## Benefits

| Aspect | Old Design | New Design |
|--------|-----------|------------|
| LLL Types | Unified with ROBBER | Two types: `lll_apth_t` and `lll_internal_t` |
| LLL Storage | `uintptr_t` (8 bytes) | Type 1: `apth_t` (8 bytes), Type 2: `unsigned char` (1 byte) |
| ROBBER | Complex robbing logic | Not needed, removed |
| `sched_of` in LLL | Required, NULL-prone | Not needed, removed |
| State | Uncommitted/committed | Simple atomic (except TERMINATED) |
| Queue Ops | Complex locking | Simple lock/modify/unlock |
| Ownership | `belongs_to_queue` atomic | `home_sched` + `current_sched` + `ownership_lock` |

## Conclusion

The new design significantly simplifies LIBAPTH's locking and state management while maintaining correctness and performance. The key insight is that the ROBBER mechanism was solving a problem that doesn't actually exist, and the uncommitted/committed protocol can be replaced with simpler lock-based atomicity for the TERMINATED state.
