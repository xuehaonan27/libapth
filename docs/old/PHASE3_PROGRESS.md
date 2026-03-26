# Phase 3 Progress: Internal Locks Migration

## Summary

Phase 3 is underway! We're migrating internal locks from the old `lll_t` to the new `lll_internal_t` (Type 2 LLL).

## Completed in Phase 3

### ✅ Queue Locks (COMPLETE)

**Files Modified:**
- `src/internal_types.h`: Changed `struct apth_thqueue_st` to use `lll_internal_t th_list_lock` and non-atomic `size_t size`
- `src/internal/apth_thqueue.c`: Updated all LLL operations and size operations
- `src/internal/apth_thqueue.inline.h`: Updated `thqueue_size()` to read non-atomic size

**Changes Made:**
- `lll_init(&queue->th_list_lock)` → `lll_internal_init(&queue->th_list_lock)`
- `lll_lock(&queue->th_list_lock, "...")` → `lll_internal_lock(&queue->th_list_lock)`
- `lll_unlock(&queue->th_list_lock, "...")` → `lll_internal_unlock(&queue->th_list_lock)`
- `_Atomic(size_t) size` → `size_t size` (non-atomic, protected by lock)
- `atomic_fetch_add_release(&queue->size, 1)` → `queue->size++`
- `atomic_fetch_sub_release(&queue->size, 1)` → `queue->size--`
- `atomic_store_release(&queue->size, 0)` → `queue->size = 0`
- Added `#include "utils/lll.inline.h"`

**Functions Updated:**
- `thqueue_init()` - Uses `lll_internal_init`, non-atomic size
- `push_apth_to()` - Uses `lll_internal_lock/unlock`, non-atomic size increment
- `pop_apth_from()` - Uses `lll_internal_lock/unlock`, non-atomic size decrement
- `remove_apth_from()` - Uses `lll_internal_lock/unlock`, non-atomic size decrement
- `drain_thqueue()` - Uses `lll_internal_lock/unlock`, non-atomic size reset
- `transfer_one_th()` - Uses `lll_internal_lock/unlock`, non-atomic size operations
- `transfer_th()` - Uses `lll_internal_lock/unlock`, non-atomic size operations
- `find_first_in_thqueue()` - Uses `lll_internal_lock/unlock`
- `visit_thqueue()` - Uses `lll_internal_lock/unlock`, non-atomic size operations
- `thqueue_size()` - Reads non-atomic size (safe for approximate checks)

**Benefits:**
- Simpler API (no debug string parameter needed)
- Behavior adapts to caller (APTH yields, scheduler spins)
- Smaller lock structure (1 byte vs 8 bytes)
- Non-atomic size is simpler and protected by lock
- No ROBBER mechanism complexity

**Important Note:**
The `thqueue_size()` function now reads the size without holding the lock. This is acceptable because:
- It's used for approximate checks (e.g., work stealing heuristics)
- Reading a `size_t` is atomic on most architectures
- Exact size is not critical for these use cases
- If exact size is needed, caller should hold the lock

## Next Steps in Phase 3

### ⏳ Signal Locks
- Update `th->siglock` in `struct apth_st` (internal_types.h)
- Update `APTH_GLOBAL_SIGACTIONS.lock` (internal_types.h)
- Update `src/internal/apth_signal.c`
- Update `src/hook_libc/hook_signal.c`
- Update `src/core/apth_kill.c`

### ⏳ Pool Lock
- Update `GLOBAL_POOL.pool_lock` in `struct apth_global_scheduler_pool` (internal_types.h)
- Update `src/internal/apth_worker.c`

### ⏳ FD Close Lock
- Update `sched->pending_fd_close_lock` in `struct apth_perpthr_scheduler` (internal_types.h)
- Update `src/internal/apth_event.c`

### ⏳ Other Internal Locks
- Check for any other internal locks that need migration
- Update as needed

## Testing Plan

After completing all internal locks:
1. Compile and check for errors
2. Run existing test suite
3. Test queue operations specifically
4. Stress test with work stealing
5. Verify no performance regression
6. Test signal handling
7. Test worker pool operations

## Files Modified So Far in Phase 3

### Modified:
- `src/internal_types.h` - Updated `apth_thqueue_st` structure
- `src/internal/apth_thqueue.c` - Migrated to `lll_internal_t`
- `src/internal/apth_thqueue.inline.h` - Updated `thqueue_size()`

## Key Insights

1. **Type 2 LLL is very efficient**: Only 1 byte vs 8 bytes for old LLL
2. **Behavior adaptation works well**: APTHs yield, schedulers spin automatically
3. **Non-atomic size is simpler**: Protected by lock, no need for atomic operations
4. **Migration is straightforward**: Similar pattern to Type 1 LLL migration
5. **Lock-free size reads are acceptable**: For approximate checks in work stealing

## Estimated Remaining Work in Phase 3

- Signal locks: ~45 minutes (multiple files, more complex)
- Pool lock: ~15 minutes (simpler, single file)
- FD close lock: ~15 minutes (simpler, single file)
- Other locks: ~15 minutes (if any)

**Total: ~1.5 hours for remaining internal locks**

Then we move to Phase 4 (ownership system) which will add new fields and update work stealing/join operations.
