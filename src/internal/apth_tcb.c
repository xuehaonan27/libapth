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
// Returns the mmap'd region pointer on hit, or NULL on miss.
static char *stack_pool_get(apth_sched_t sched, size_t total_size)
{
    if (sched == NULL)
        return NULL;
    for (int i = 0; i < sched->stack_pool_count; i++)
    {
        if (sched->stack_pool[i].size == total_size)
        {
            char *mem = sched->stack_pool[i].mem;
            // Swap with last entry and shrink pool
            sched->stack_pool_count--;
            if (i < sched->stack_pool_count)
                sched->stack_pool[i] = sched->stack_pool[sched->stack_pool_count];
            apth_debug("stack pool hit: reusing %zu byte stack at %p", total_size, mem);
            return mem;
        }
    }
    return NULL;
}

// Return a stack to the pool. If pool is full, munmap it.
static void stack_pool_put(apth_sched_t sched, char *mem, size_t total_size)
{
    if (sched == NULL || sched->stack_pool_count >= APTH_STACK_POOL_MAX)
    {
        // Pool full or no scheduler — release back to OS
        munmap(mem, total_size);
        return;
    }
    // Re-protect the guard page region as read-write before caching,
    // so the next user of this stack can re-protect cleanly.
    // (mprotect is idempotent for already-RW pages, cheap for guard pages)
    mprotect(mem, total_size, PROT_READ | PROT_WRITE);

    int idx = sched->stack_pool_count++;
    sched->stack_pool[idx].mem = mem;
    sched->stack_pool[idx].size = total_size;
    apth_debug("stack pool put: cached %zu byte stack at %p (pool=%d)",
               total_size, mem, sched->stack_pool_count);
}

APTH_INTERNAL apth_t apth_tcb_alloc(size_t stacksize, void *stackaddr, size_t guardsize)
{
    apth_t t;

    if (stacksize <= 0)
        return APTH_NULL;
    if (stacksize < APTH_MIN_STACK)
        stacksize = APTH_MIN_STACK;
    if ((t = (apth_t)malloc(sizeof(struct apth_st))) == NULL)
        return APTH_NULL;

    assert_msg(APTH_ALIGNED_ASSURE(t), "t not aligned to 4 bytes: %p", t);

    memset(t, '\0', sizeof(struct apth_st));

    t->stacksize = stacksize;
    t->guardsize = guardsize;
    t->stack_mem_start = NULL;
    t->magic = APTH_MAGIC; // Set magic number for validation
    t->stackloan = (stackaddr != NULL ? true : false);
    list_init(&t->event_list);

    if (t->stackloan)
    {
        // User-provided stack - we don't set up guard pages for loaned stacks
        // as we don't control the memory allocation
#if APTH_STACKGROWTH < 0
        t->stack_mem_start = (char *)stackaddr - stacksize;
#else
        t->stack_mem_start = (char *)stackaddr;
#endif
        t->guardsize = 0; // No guard page for loaned stacks
    }
    else
    {
        // Allocate our own stack with guard page support
        size_t total_size = stacksize;

        if (guardsize > 0)
        {
            // Round guardsize up to page boundary
            size_t pagesize = page_size();
            size_t guard_pages = (guardsize + pagesize - 1) / pagesize;
            guardsize = guard_pages * pagesize;
            t->guardsize = guardsize;
            total_size += guardsize;
        }

        // Try the scheduler's stack pool first
        apth_sched_t sched = CUR_SCHED;
        char *mem = stack_pool_get(sched, total_size);

        if (mem == NULL)
        {
            // Pool miss — allocate from OS
            mem = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mem == MAP_FAILED)
            {
                apth_shield { free(t); }
                return APTH_NULL;
            }
        }

        t->stack_mem_start = mem;

        // Set up guard page if requested
        if (guardsize > 0)
        {
#if APTH_STACKGROWTH < 0
            // Stack grows downward: guard page at the lowest address
            if (mprotect(t->stack_mem_start, guardsize, PROT_NONE) != 0)
            {
                apth_debug("Warning: mprotect failed for guard page: %s", strerror(errno));
            }
#else
            // Stack grows upward: guard page at the highest address
            if (mprotect(t->stack_mem_start + stacksize, guardsize, PROT_NONE) != 0)
            {
                apth_debug("Warning: mprotect failed for guard page: %s", strerror(errno));
            }
#endif
        }
    }
    return t;
}

// Get the usable stack start address (after guard page if present)
APTH_INTERNAL char *apth_tcb_get_usable_stack_start(apth_t t)
{
    if (t->guardsize > 0)
    {
#if APTH_STACKGROWTH < 0
        // Stack grows downward: usable stack starts after the guard page
        return t->stack_mem_start + t->guardsize;
#else
        // Stack grows upward: usable stack starts at the beginning
        return t->stack_mem_start;
#endif
    }
    else
    {
        // No guard page
        return t->stack_mem_start;
    }
}

APTH_INTERNAL void apth_tcb_free(apth_t t)
{
    assert(t != NULL);
    apth_sched_t sched = CUR_SCHED;

    if (t->stack_mem_start != NULL && !t->stackloan)
    {
        // Return stack to pool instead of munmap
        size_t total_size = t->stacksize + t->guardsize;
        stack_pool_put(sched, t->stack_mem_start, total_size);
    }

    assert_msg(t->cleanups == NULL, "apth %p try to TCB free without executing cleanups", t);

    // Clear magic number to invalidate the TCB
    t->magic = 0;

    free(t);

    // Decrement
    dec_thrcnt(sched);
}
