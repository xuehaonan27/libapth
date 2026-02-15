#include "internal_types.h"
#include "utils/archplattoold.h"
#include "utils/apth_errno.h"
#include "utils/debug.h"
#include <malloc.h>
#include <string.h>

// Save the current thread context into `ctx`.
APTH_INTERNAL bool apth_ctx_save(apth_cxt_t ctx)
{
    ctx->error = errno;
    ctx->restored = 0;
    getcontext(&ctx->uc);
    return ctx->restored;
}

// Restore the current machine context (at the location of the old context)
APTH_INTERNAL void apth_ctx_restore(apth_cxt_t ctx)
{
    errno = ctx->error;
    ctx->restored = 1;
    setcontext(&ctx->uc);
}

// Restore the current machine context (at the location of the new context)
// APTH_INTERNAL void apth_ctx_restored(apth_cxt_t ctx)
// {
//     // TODO: pth_sc(sigprocmask)(SIG_SETMASK, &((mctx)->sigs), NULL)
// }

#define APTH_SWITCH_DEBUG_LINE \
    "==== THREAD CONTEXT SWITCH ==========================================="

#ifdef APTH_DEBUG
#define _apth_mctx_switch_debug apth_debug(NULL, 0, 1, APTH_SWITCH_DEBUG_LINE);
#else
#define _apth_mctx_switch_debug /* NOP */
#endif

APTH_INTERNAL apth_cxt_t apth_ctx_alloc(void)
{
    apth_cxt_t ctx = (apth_cxt_t)malloc(sizeof(struct apth_cxt_st));
    memset(ctx, '\0', sizeof(struct apth_cxt_st));
    return ctx;
}

// Swap context from `old` to `new`.
APTH_INTERNAL void apth_ctx_switch(apth_cxt_t old, apth_cxt_t new)
{
    _apth_mctx_switch_debug;
    swapcontext(&old->uc, &new->uc);
}

// #define apth_skaddr_makecontext(skaddr, sksize) ((skaddr))
// #define apth_sksize_makecontext(skaddr, sksize) ((sksize))

// Initialize a context into `ctx`.
APTH_INTERNAL bool apth_ctx_set(apth_cxt_t ctx, void (*func)(void), char *stack_addr_lo, char *stack_addr_hi)
{
    apth_debug("enter");
    // fetch current context
    if (getcontext(&ctx->uc) != 0)
        return false;

    apth_debug("got context");

    // remove parent link
    ctx->uc.uc_link = NULL;

    // configure new stack
    ctx->uc.uc_stack.ss_sp = stack_addr_hi;
    ctx->uc.uc_stack.ss_size = stack_addr_hi - stack_addr_lo;
    ctx->uc.uc_stack.ss_flags = 0;

    // configure startup function (with no arguments)
    makecontext(&ctx->uc, func, 1);

    apth_debug("leave");
    return true;
}
