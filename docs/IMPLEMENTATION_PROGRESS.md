# Implementation Progress: New LLL and State Management

## Completed

### 1. New LLL Type Definitions
- ✅ Created `src/utils/lll_new.h` with Type 1 and Type 2 LLL definitions
- ✅ Created `src/utils/lll_new.inline.h` with complete implementations
- ✅ Type 1 LLL (`lll_apth_t`): For synchronization primitives, APTH-only
- ✅ Type 2 LLL (`lll_internal_t`): For internal locks, mixed APTH/scheduler

### 2. Updated Type Definitions
- ✅ Added comments to `internal_types.h` showing new fields for:
  - APTH structure: `state`, `home_sched`, `current_sched`, `current_queue`, `ownership_lock`
  - Queue structure: `lock` (Type 2 LLL), `size` (non-atomic)
  - Mutex structure: `guard` (Type 1 LLL)
  - Cond structure: `guard` (Type 1 LLL)
  - Sem structure: `guard` (Type 1 LLL)
  - Rwlock structure: `guard` (Type 1 LLL)
  - Barrier structure: `guard` (Type 1 LLL)
  - Signal lock: `siglock` (Type 2 LLL)
  - Global signal actions: `lock` (Type 2 LLL)
  - Pool lock: `pool_lock` (Type 2 LLL)
  - FD close lock: `pending_fd_close_lock` (Type 2 LLL)

### 3. Documentation
- ✅ Created `docs/NEW_LLL_AND_STATE_DESIGN.md` with:
  - Complete design rationale
  - Implementation details
  - Migration plan
  - Code examples
  - Benefits comparison

## Key Design Decisions

### Type 1 LLL (Synchronization Primitives)
```c
typedef struct {
    _Atomic(apth_t) owner;  // NULL or APTH pointer
} lll_apth_t;
```
- Stores APTH pointer (8 bytes)
- Only APTHs acquire
- Yields on contention
- Provides `trylock`

### Type 2 LLL (Internal LIBAPTH)
```c
typedef struct {
    _Atomic(unsigned char) locked;  // 0 or 1
} lll_internal_t;
```
- Stores only flag (1 byte)
- APTHs and schedulers can acquire
- Behavior depends on caller:
  - APTH: yields
  - Scheduler: spins
- No `trylock` needed

### Simplified State Management
- Remove uncommitted/committed for most states
- Use simple atomic stores
- Special case TERMINATED: change state while holding queue lock
- Add ownership system: `home_sched`, `current_sched`, `ownership_lock`

## Next Steps

### Phase 2: Update Synchronization Primitives
1. Update `src/core/apth_mutex.c` to use `lll_apth_t`
2. Update `src/core/apth_cond.c` to use `lll_apth_t`
3. Update `src/core/apth_sem.c` to use `lll_apth_t`
4. Update `src/core/apth_rwlock.c` to use `lll_apth_t`
5. Update `src/core/apth_barrier.c` to use `lll_apth_t`

### Phase 3: Update Internal Locks
1. Update `src/internal/apth_thqueue.c` to use `lll_internal_t`
2. Update `src/internal/apth_signal.c` to use `lll_internal_t`
3. Update `src/internal/apth_worker.c` to use `lll_internal_t`
4. Update `src/internal/apth_fd.c` to use `lll_internal_t`

### Phase 4: Add Ownership System
1. Uncomment new fields in `internal_types.h`
2. Initialize in `apth_create`
3. Update work stealing in `apth_sched.c`
4. Update join in `apth_join.c`

### Phase 5: Simplify State Management
1. Add simple `state` field
2. Update state transitions
3. Special-case TERMINATED
4. Update event manager

### Phase 6: Remove Old Code
1. Remove `state_holder`
2. Remove `belongs_to_queue`
3. Remove old `lll.h` and `lll.inline.h`
4. Remove ROBBER mechanism

## Testing Strategy

1. **Unit Tests**: Test each LLL type independently
2. **Integration Tests**: Test state transitions and queue operations
3. **Stress Tests**: Heavy load on work stealing and join
4. **Race Detection**: Run with ThreadSanitizer
5. **Performance**: Benchmark against old implementation

## Files Created/Modified

### Created:
- `src/utils/lll_new.h`
- `src/utils/lll_new.inline.h`
- `docs/NEW_LLL_AND_STATE_DESIGN.md`
- `docs/IMPLEMENTATION_PROGRESS.md` (this file)

### Modified:
- `src/internal_types.h` (added comments for new fields)

## How to Proceed

The foundation is now in place. The next step is to start migrating actual code to use the new LLL types. I recommend starting with a single synchronization primitive (e.g., mutex) as a proof of concept, then expanding to others.

Would you like me to:
1. Start implementing Phase 2 (update synchronization primitives)?
2. Create a test file to verify the new LLL implementations?
3. Start implementing the ownership system?
4. Something else?
