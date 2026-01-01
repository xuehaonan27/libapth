#include "internal_types.h"

/* save the current thread context */
bool apth_ctx_save(apth_cxt_t ctx)
{
    ctx->error = errno;
    ctx->restored = 0;
    getcontext(&ctx->uc);
    return ctx->restored;
}

/*  restore the current machine context
    (at the location of the old context) */
void apth_ctx_restore(apth_cxt_t ctx)
{
    errno = ctx->error;
    ctx->restored = 1;
    setcontext(&ctx->uc);
}

/* restore the current machine context
   (at the location of the new context)*/
void apth_ctx_restored(apth_cxt_t ctx)
{
    // TODO: pth_sc(sigprocmask)(SIG_SETMASK, &((mctx)->sigs), NULL)
}

#define APTH_SWITCH_DEBUG_LINE \
    "==== THREAD CONTEXT SWITCH ==========================================="

#ifdef APTH_DEBUG
#define _apth_mctx_switch_debug apth_debug(NULL, 0, 1, APTH_SWITCH_DEBUG_LINE);
#else
#define _apth_mctx_switch_debug /* NOP */
#endif

void apth_ctx_switch(apth_cxt_t old, apth_cxt_t new)
{
    _apth_mctx_switch_debug;
    swapcontext(&old->uc, &new->uc);
}

#define apth_skaddr_makecontext(skaddr, sksize) ((skaddr))
#define apth_sksize_makecontext(skaddr, sksize) ((sksize))

bool apth_ctx_set(apth_cxt_t ctx, void (*func)(void), char *stack_addr_lo, char *stack_addr_hi)
{
    // fetch current context
    if (getcontext(&ctx->uc) != 0)
        return false;

    // remove parent link
    ctx->uc.uc_link = NULL;

    // configure new stack
    ctx->uc.uc_stack.ss_sp = apth_skaddr_makecontext(stack_addr_lo, stack_addr_hi - stack_addr_lo);
    ctx->uc.uc_stack.ss_size = apth_sksize_makecontext(stack_addr_lo, stack_addr_hi - stack_addr_lo);
    ctx->uc.uc_stack.ss_flags = 0;

    // configure startup function (with no arguments)
    makecontext(&ctx->uc, func, 1);

    return true;
}