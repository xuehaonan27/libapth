# Low-Level Lock (LLL) Implementation Notes

## Overview

This document describes the implementation of the low-level lock (lll) for the libapth userspace thread library.

## Design Philosophy

The low-level lock is designed to provide synchronization between multiple apth (userspace) threads with minimal overhead by:

1. **Using atomic operations** for lock acquisition and release
2. **Encoding ownership information** in the lock state using pointer tagging
3. **Cooperating with the scheduler** to avoid busy-waiting and deadlocks

## Lock State Encoding

The lock uses a single atomic `uintptr_t` to store the owner's TCB pointer directly:

```
Lock Value = TCB pointer (apth_t) or 0 if not acquired
```

This simple encoding makes the lock implementation very efficient:
- **0 (LLL_NOT_ACQUIRED)**: Lock is free, no owner
- **Non-zero**: The pointer to the owner's TCB (apth_t)

Since TCB structures are guaranteed to be at least 4-byte aligned, we could use the lower bits for additional information, but **we found this is unnecessary**. Each waiter can determine its relationship to the owner locally without storing it in the lock.

## Implementation Details

### lll_lock()

The lock acquisition follows a two-phase approach:

#### Fast Path
1. Attempt to acquire the lock using a single CAS operation
2. If the lock is free (value is 0), atomically set it to the calling thread's TCB pointer (`self_ptr`)
3. Return immediately if successful

#### Slow Path (Contention)
1. **Load current lock value** with acquire semantics to get the owner
2. **If lock is free**, retry acquisition with CAS
3. **If lock is held**:
   - Determine if the owner is on the same worker as the current thread
   - Search through all workers to find where the owner is currently running
   - Make a local decision about yielding strategy:
     - If **same worker**: MUST yield to avoid deadlock (only one apth runs per worker)
     - If **different worker**: Can yield immediately (owner is on another CPU)
4. **Yield to scheduler** to give the owner or other threads a chance to run
5. **Retry** after yielding

**Why we don't store waiter status in the lock:**
- Each waiter's relationship to the owner is dynamic and local
- Storing it would require extra CAS operations
- The stored state could be stale for the next waiter
- Making the decision locally is simpler and equally effective

### lll_unlock()

The unlock operation is straightforward:

1. **Load the current lock value** (relaxed semantics)
2. **Verify ownership** by checking if the owner TCB pointer matches the calling thread
3. **Release the lock** by atomically storing 0 with release semantics

The release semantics ensure that all memory operations performed while holding the lock are visible to the next thread that acquires the lock.

**Ownership verification** is important for debugging - if a thread tries to unlock a lock it doesn't own, this indicates a serious programming error.

## Key Features

### 1. Deadlock Avoidance
When a thread on the same worker tries to acquire a lock held by another thread on the same worker, it MUST yield. This prevents deadlock since only one apth can run at a time on a worker.

### 2. Minimal Overhead
- Fast path requires only one atomic CAS operation
- No system calls in the common case
- Uses scheduler cooperation instead of blocking system calls

### 3. Fairness Considerations
The current implementation uses a simple spin-and-yield strategy. While this doesn't guarantee strict fairness, it provides good performance in low-contention scenarios.

### 4. Memory Ordering
- **Acquire semantics** on lock acquisition ensures visibility of previous writes by the previous lock holder
- **Release semantics** on unlock ensures visibility of all operations performed under the lock to the next acquirer
- **Relaxed semantics** for ownership checks that don't require synchronization

## Performance Characteristics

### Best Case (No Contention)
- **Lock**: 1 atomic CAS operation
- **Unlock**: 1 atomic store operation

### Worst Case (High Contention)
- Multiple CAS attempts in the retry loop
- Yields to scheduler, allowing other threads to run
- No busy-waiting, minimizing CPU waste

## Future Optimizations

Potential areas for optimization:

1. **Adaptive spinning**: Spin for a short period before yielding on cross-worker contention
2. **Priority inheritance**: Boost the priority of lock holders to reduce wait time
3. **Lock statistics**: Track contention metrics for debugging and optimization
4. **NUMA awareness**: Consider memory locality in worker selection

## Usage Example

```c
#include "utils/lll.h"

// Declare a lock (typically as part of a larger structure)
lll_t my_lock = { .inner = LLL_NOT_ACQUIRED };

// In thread code:
lll_lock(&my_lock);
// Critical section - only one apth can execute this at a time
// ... protected operations ...
lll_unlock(&my_lock);
```

## Thread Safety

The implementation is thread-safe across multiple apth threads and multiple pthread workers. The atomic operations ensure that lock state transitions are consistent even under concurrent access.

## Debugging

In debug builds, the unlock function verifies that the calling thread actually owns the lock. This helps catch programming errors early.

## Conclusion

This low-level lock implementation provides an efficient, lightweight synchronization primitive tailored to the userspace threading model of libapth. By leveraging pointer tagging, atomic operations, and scheduler cooperation, it minimizes overhead while avoiding deadlocks and busy-waiting.
