#include "apth_tcb.h"
#include "internal/apth_sched.h"
#include "internal/types.h"
#include "utils/apth_errno.h"
#include "utils/apth_sysutils.h"
#include "utils/atomic_wrapper.h"
#include "utils/debug.h"
#include "utils/list.inline.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// The most lower 2 bits should be 0, meaning TCB should be at least 4 bytes aligned.
#define APTH_ALIGNED_ASSURE(t) (((uintptr_t)(t) & 0x3) == 0)

// Try to get a cached stack from the scheduler's stack pool.
// Accepts any cached stack with size >= total_size (wasteful up to 2x
// is acceptable to avoid a mmap syscall + TLB shootdown).
static char *stack_pool_get(apth_sched_t sched, size_t total_size, size_t *actual_size)
{
    if (sched == NULL)
        return NULL;

    int best_idx = -1;
    size_t best_size = (size_t)-1;

    for (int i = 0; i < sched->stack_pool_count; i++)
    {
        size_t sz = sched->stack_pool[i].size;
        if (sz >= total_size && sz < best_size)
        {
            best_idx = i;
            best_size = sz;
            if (sz == total_size)
                break; // Exact match, no need to search further
        }
    }

    if (best_idx < 0)
        return NULL;

    char *mem = sched->stack_pool[best_idx].mem;
    *actual_size = sched->stack_pool[best_idx].size;

    // Swap with last entry and shrink pool
    sched->stack_pool_count--;
    if (best_idx < sched->stack_pool_count)
        sched->stack_pool[best_idx] = sched->stack_pool[sched->stack_pool_count];

    apth_debug("stack pool hit: reusing %zu byte stack at %p (requested %zu)",
               *actual_size, mem, total_size);
    return mem;
}

// Return a stack to the pool.  If pool is full, release back to OS.
// Uses MADV_DONTNEED instead of munmap to release physical pages without
// unmapping the virtual address space, avoiding TLB shootdowns and making
// the next reuse a cheap page fault instead of a full mmap.
static void stack_pool_put(apth_sched_t sched, char *mem, size_t total_size)
{
    if (sched == NULL || sched->stack_pool_count >= APTH_STACK_POOL_MAX)
    {
        // Pool full or no scheduler — release back to OS
        munmap(mem, total_size);
        return;
    }

    // Release physical pages but keep the virtual mapping.
    // The guard page protection is preserved across MADV_DONTNEED,
    // so we don't need to re-mprotect on reuse.
    // On reuse, faulting in fresh zero pages is cheaper than mmap+mprotect.
    madvise(mem, total_size, MADV_DONTNEED);

    int idx = sched->stack_pool_count++;
    sched->stack_pool[idx].mem = mem;
    sched->stack_pool[idx].size = total_size;
    apth_debug("stack pool put: cached %zu byte stack at %p (pool=%d)",
               total_size, mem, sched->stack_pool_count);
}

// Align size up to 'align' boundary (align must be power of 2).
static size_t align_up(size_t val, size_t align)
{
    return (val + align - 1) & ~(align - 1);
}

APTH_INTERNAL apth_t apth_tcb_alloc(size_t stacksize, void *stackaddr, size_t guardsize)
{
    if (stacksize <= 0)
        return APTH_NULL;
    if (stacksize < APTH_MIN_STACK)
        stacksize = APTH_MIN_STACK;

    if (stackaddr != NULL)
    {
        // User-provided (loaned) stack — TCB must be separate malloc.
        apth_t t;
        if ((t = (apth_t)malloc(sizeof(struct apth_st))) == NULL)
            return APTH_NULL;
        memset(t, '\0', sizeof(struct apth_st));
        t->stacksize = stacksize;
        t->guardsize = 0;
        t->magic = APTH_MAGIC;
        t->stackloan = true;
        t->tcb_merged = false;
        list_init(&t->event_list);
#if APTH_STACKGROWTH < 0
        t->stack_mem_start = (char *)stackaddr - stacksize;
#else
        t->stack_mem_start = (char *)stackaddr;
#endif
        return t;
    }

    // Merged allocation: single mmap for guard + stack + TCB.
    // Layout (downward-growing stack):
    //   [guard page (PROT_NONE)] [stack space ...] [TCB (64-byte aligned)]
    // The TCB sits at the high end; the stack grows down from just below it.
    size_t tcb_size = align_up(sizeof(struct apth_st), 64);
    size_t pagesize = page_size();

    if (guardsize > 0)
    {
        size_t guard_pages = (guardsize + pagesize - 1) / pagesize;
        guardsize = guard_pages * pagesize;
    }

    size_t total_size = guardsize + stacksize + tcb_size;
    // Round up to page boundary for mmap
    total_size = align_up(total_size, pagesize);

    // Try the scheduler's stack pool first
    apth_sched_t sched = CUR_SCHED;
    size_t actual_size = 0;
    char *mem = stack_pool_get(sched, total_size, &actual_size);
    bool from_pool = (mem != NULL);

    if (from_pool)
    {
        total_size = actual_size;
    }
    else
    {
        mem = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED)
            return APTH_NULL;

        if (guardsize > 0)
        {
#if APTH_STACKGROWTH < 0
            if (mprotect(mem, guardsize, PROT_NONE) != 0)
            {
                apth_debug("mprotect failed for guard page: %s", strerror(errno));
                munmap(mem, total_size);
                return APTH_NULL;
            }
#else
            if (mprotect(mem + stacksize + tcb_size, guardsize, PROT_NONE) != 0)
            {
                apth_debug("mprotect failed for guard page: %s", strerror(errno));
                munmap(mem, total_size);
                return APTH_NULL;
            }
#endif
        }
    }

    // Place TCB at the high end of the mapping
    apth_t t = (apth_t)(mem + total_size - tcb_size);
    assert_msg(APTH_ALIGNED_ASSURE(t), "t not aligned to 4 bytes: %p", t);

    memset(t, '\0', sizeof(struct apth_st));
    t->stack_mem_start = mem;
    t->stacksize = total_size - guardsize - tcb_size;
    t->guardsize = guardsize;
    t->magic = APTH_MAGIC;
    t->stackloan = false;
    t->tcb_merged = true;
    list_init(&t->event_list);

    return t;
}

// Get the usable stack start address (after guard page if present).
// For merged TCB: stack is between guard page and TCB.
APTH_INTERNAL char *apth_tcb_get_usable_stack_start(apth_t t)
{
#if APTH_STACKGROWTH < 0
    // Stack grows downward: usable stack starts after the guard page
    return t->stack_mem_start + t->guardsize;
#else
    // Stack grows upward: usable stack starts at the beginning
    return t->stack_mem_start;
#endif
}

APTH_INTERNAL void apth_tcb_free(apth_t t)
{
    assert(t != NULL);
    apth_sched_t sched = CUR_SCHED;

    assert_msg(t->cleanups == NULL, "apth %p try to TCB free without executing cleanups", t);

    // Clear magic number to invalidate the TCB
    t->magic = 0;

    if (t->tcb_merged)
    {
        // TCB is embedded in the stack mmap — return entire region to pool or OS.
        char *mem = t->stack_mem_start;
        size_t tcb_size = align_up(sizeof(struct apth_st), 64);
        size_t total_size = t->stacksize + t->guardsize + tcb_size;
        // Pool or munmap the entire mmap region.
        apth_sched_t pool_sched = (sched != NULL && sched->id >= 0) ? sched : NULL;
        stack_pool_put(pool_sched, mem, total_size);
    }
    else
    {
        // Separate stack and TCB allocations.
        if (t->stack_mem_start != NULL && !t->stackloan)
        {
            apth_sched_t pool_sched = (sched != NULL && sched->id >= 0) ? sched : NULL;
            size_t total_size = t->stacksize + t->guardsize;
            stack_pool_put(pool_sched, t->stack_mem_start, total_size);
        }
        free(t);
    }

    // Decrement
    dec_thrcnt(sched);
}
