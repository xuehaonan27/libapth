# Guard Page Implementation for LIBAPTH

## Summary

This document describes the implementation of hardware-based stack overflow protection using guard pages in LIBAPTH, replacing the previous manual stack overflow detection mechanism.

## Changes Made

### 1. Modified `struct apth_st` (internal_types.h)

**Before:**
```c
char *stack_mem_start;       // pointer to thread stack
size_t stacksize;            // size of thread stack
uint32_t *stackguard;        // stack overflow guard
```

**After:**
```c
char *stack_mem_start;       // pointer to thread stack memory (including guard page if any)
size_t stacksize;            // size of thread stack (excluding guard page)
size_t guardsize;            // size of guard page
uint32_t magic;              // magic number for validation (replaces stackguard)
```

### 2. Updated `APTH_IS_VALID` Macro (internal_types.h)

**Before:**
```c
#define APTH_TH_MAGIC_IS_GOOD(th) (*(uint32_t *)((th)->stackguard) == APTH_MAGIC)
```

**After:**
```c
#define APTH_TH_MAGIC_IS_GOOD(th) ((th)->magic == APTH_MAGIC)
```

The magic value is now embedded directly in the TCB structure instead of being stored at a stack address.

### 3. Reimplemented `apth_tcb_alloc()` (internal/apth_tcb.c)

**Key Changes:**
- Added `guardsize` parameter
- Uses `mmap()` instead of `malloc()` for stack allocation
- Allocates `stacksize + guardsize` bytes total
- Uses `mprotect(PROT_NONE)` to make guard page inaccessible
- Guard page placement depends on stack growth direction:
  - **Downward growth** (`APTH_STACKGROWTH < 0`): Guard at lowest address
  - **Upward growth** (`APTH_STACKGROWTH > 0`): Guard at highest address
- Sets `t->magic = APTH_MAGIC` directly in TCB
- For user-provided stacks (loaned), no guard page is set

### 4. Updated `apth_tcb_free()` (internal/apth_tcb.c)

**Changes:**
- Uses `munmap()` instead of `free()` for stack deallocation
- Unmaps `stacksize + guardsize` bytes
- Clears magic number (`t->magic = 0`) to invalidate TCB

### 5. Added Helper Function (internal/apth_tcb.c)

```c
APTH_INTERNAL char *apth_tcb_get_usable_stack_start(apth_t t)
```

Returns the start address of the usable stack area (after the guard page if present).

### 6. Updated `apth_create()` (core/apth_create.c)

**Changes:**
- Passes `iattr->guardsize` to `apth_tcb_alloc()`
- Uses `apth_tcb_get_usable_stack_start()` when setting up context

### 7. Removed Manual Stack Overflow Check (internal/apth_sched.c)

**Removed:**
- Manual check of `stackguard` magic value in scheduler loop
- SIGSEGV generation code for detected overflows
- Unused variables `sa` and `ss`

**Replaced with:**
```c
// Note: Stack overflow detection is now handled by guard pages.
// If a thread overflows its stack, it will trigger a SIGSEGV when
// accessing the protected guard page, which is handled by the OS.
```

### 8. Updated Context Initialization (internal/apth_ctx.c)

Added documentation clarifying that `stack_mem_start` should point to the usable stack area (after guard page).

## How It Works

### Guard Page Mechanism

1. **Allocation**: When a thread is created, LIBAPTH allocates memory using `mmap()`:
   ```
   Total allocation = stacksize + guardsize
   ```

2. **Protection**: The guard page region is protected using `mprotect(PROT_NONE)`:
   - No read access
   - No write access
   - No execute access

3. **Stack Layout** (for downward-growing stacks):
   ```
   Low Address
   ┌─────────────────┐
   │  Guard Page     │ ← Protected with PROT_NONE
   │  (guardsize)    │
   ├─────────────────┤
   │                 │
   │  Usable Stack   │ ← Thread's stack grows down into this
   │  (stacksize)    │
   │                 │
   └─────────────────┘
   High Address
   ```

4. **Overflow Detection**: When a thread overflows its stack and tries to access the guard page:
   - Hardware MMU detects the access violation
   - OS delivers SIGSEGV to the process
   - No manual checking required in scheduler

### Default Guard Size

- Default: 1 page (typically 4096 bytes on x86-64)
- Set in `apth_attr_init()`: `iattr->guardsize = page_size();`
- Can be customized via `apth_attr_setguardsize()`
- Can be disabled by setting guardsize to 0

## Benefits

1. **Hardware Protection**: Uses MMU for detection, more reliable than manual checks
2. **Zero Runtime Overhead**: No need to check magic value on every context switch
3. **Immediate Detection**: Overflow is detected at the exact moment it occurs
4. **Standard Behavior**: Matches pthread behavior for guard pages
5. **Cleaner Code**: Removes manual checking logic from scheduler

## Compatibility

- **User-Provided Stacks**: Guard pages are NOT set for loaned stacks (when user provides stackaddr)
- **Attribute Control**: Respects `apth_attr_setguardsize()` - setting to 0 disables guard page
- **Signal Handling**: Applications can install SIGSEGV handlers to catch stack overflows

## Testing

A test program (`test/test_guard_page.c`) has been created to verify the guard page mechanism triggers SIGSEGV on stack overflow.

## Future Enhancements

1. **Configurable Guard Size**: Already supported via attributes
2. **Multiple Guard Pages**: Could allocate multiple pages for extra protection
3. **Stack Usage Monitoring**: Could use `/proc/self/maps` to monitor actual stack usage
4. **Alternate Signal Stack**: Could set up alternate signal stack for SIGSEGV handler

## Notes

- The implementation assumes `APTH_STACKGROWTH < 0` (downward-growing stacks) which is standard on x86-64 Linux
- Guard pages use virtual memory, so they don't consume physical RAM
- The magic number is still used for TCB validation via `APTH_IS_VALID()` macro
