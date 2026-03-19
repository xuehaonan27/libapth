#include "apth_ctx.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_cancel.h"
#include "utils/atomic_wrapper.h"
#include "utils/archplattoold.h"
#include "utils/debug.h"
#include <malloc.h>
#include <string.h>
#include <stdbool.h>

// Save the current thread context into `ctx`.
APTH_INTERNAL void apth_ctx_save(apth_cxt_t ctx)
{
    ctx->error = errno;
    getcontext(&ctx->uc);
}

// Restore the current machine context (at the location of the old context)
APTH_INTERNAL void apth_ctx_restore(apth_cxt_t ctx)
{
    errno = ctx->error;
    setcontext(&ctx->uc);
}

#define APTH_SWITCH_DEBUG_LINE \
    "==== THREAD CONTEXT SWITCH ==========================================="

// Swap context from `old` to `new`.
APTH_INTERNAL void apth_ctx_switch(apth_cxt_t old, apth_cxt_t new)
{
    apth_debug(APTH_SWITCH_DEBUG_LINE);
    old->error = errno;
    swapcontext(&old->uc, &new->uc);
    errno = new->error;

    // After the context has been switched
    apth_t cur = CUR_APTH;
    if (cur != NULL)
    {
        unsigned int cc_h = atomic_load_acquire(&cur->cancelhandling);
        // When cancellation is enabled in async mode we cancel the thread immediately
        if (atomic_load_acquire(&cur->cancelreq) /* Have request */
            && (cc_h & CANCELSTATE_BITMASK) == 0 /* Cancel enabled */
            && (cc_h & CANCELTYPE_BITMASK) != 0 /* Asynchronous */)
        {
            apth_cancel_point();
        }
    }
}

// Initialize a context into `ctx`.
// Note: stack_mem_start should point to the beginning of the usable stack area
// (after the guard page if present), and stacksize should be the usable stack size
// (excluding the guard page).
APTH_INTERNAL bool apth_ctx_set(apth_cxt_t ctx, void (*func)(void),
                                char *stack_mem_start, size_t stacksize)
{
    apth_debug("enter");
    // fetch current context
    if (getcontext(&ctx->uc) != 0)
        return false;

    // remove parent link
    ctx->uc.uc_link = NULL;

    // configure new stack
    // note: according to manual of ucontext, it's not for the program to consider
    // the growth direction of the stack.
    ctx->uc.uc_stack.ss_sp = stack_mem_start;
    apth_debug("STACK memory start at = %p", stack_mem_start);
    ctx->uc.uc_stack.ss_size = stacksize;
    ctx->uc.uc_stack.ss_flags = 0;

    // configure startup function (with no arguments)
    makecontext(&ctx->uc, func, 0);

    apth_debug("leave");
    return true;
}
