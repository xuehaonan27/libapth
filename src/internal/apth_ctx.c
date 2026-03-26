#include "apth_ctx.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_cancel.h"
#include "utils/atomic_wrapper.h"
#include "utils/archplattoold.h"
#include "utils/debug.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h> /* abort */

#define APTH_SWITCH_DEBUG_LINE \
    "==== THREAD CONTEXT SWITCH ==========================================="

/*
 * Swap context from old to new using the assembly fast path.
 *
 * Saves/restores errno around the switch.  After the assembly routine
 * returns (i.e. when we are resumed), old->error still contains the
 * errno we saved before switching away, because the compiler preserves
 * the 'old' pointer across the call (it's in a callee-saved register
 * or spilled to our stack, both of which survive the switch).
 */
APTH_INTERNAL void apth_ctx_switch(apth_cxt_t old, apth_cxt_t new)
{
    apth_debug(APTH_SWITCH_DEBUG_LINE);
    old->error = errno;

    apth_ctx_switch_asm(&old->sp, new->sp);

    /* Resumed — restore our errno */
    errno = old->error;

    /* After the context has been switched back to us */
    apth_t cur = CUR_APTH;
    if (cur != NULL)
    {
        unsigned int cc_h = atomic_load_acquire(&cur->cancelhandling);
        /* When cancellation is enabled in async mode we cancel immediately */
        if (atomic_load_acquire(&cur->cancelreq)   /* Have request */
            && (cc_h & CANCELSTATE_BITMASK) == 0    /* Cancel enabled */
            && (cc_h & CANCELTYPE_BITMASK) != 0)    /* Asynchronous */
        {
            apth_cancel_point();
        }
    }
}

/*
 * Set up a fresh context for a new thread.
 *
 * Build a stack frame that looks like apth_ctx_switch_asm just saved
 * its registers, so that the first switch-to will pop the registers
 * and "return" into func.
 *
 * Stack layout (growing downward, high addresses first):
 *
 *   stack_top  (16-byte aligned)
 *   -8   sentinel return address (abort — func must never return)
 *  -16   func address            (popped by 'ret' in the asm routine)
 *  -24   saved rbp  = 0
 *  -32   saved rbx  = 0
 *  -40   saved r12  = 0
 *  -48   saved r13  = 0
 *  -56   saved r14  = 0
 *  -64   saved r15  = 0   <-- ctx->sp points here
 *
 * When apth_ctx_switch_asm restores this context:
 *   1. rsp = ctx->sp  (stack_top - 64)
 *   2. pop r15..rbp   (rsp -> stack_top - 16)
 *   3. ret             pops func address, rsp -> stack_top - 8
 *
 * At func's entry, rsp = stack_top - 8 ≡ 8 (mod 16), satisfying the
 * SysV ABI requirement (rsp % 16 == 8 at function entry, because a
 * normal CALL would have pushed the return address).
 */
APTH_INTERNAL bool apth_ctx_set(apth_cxt_t ctx, void (*func)(void),
                                char *stack_mem_start, size_t stacksize)
{
    apth_debug("enter");

    if (stack_mem_start == NULL || stacksize < 128)
        return false;

    /* Calculate the usable stack top (stack grows downward on x86-64) */
    uintptr_t stack_top = (uintptr_t)stack_mem_start + stacksize;

    /* Align to 16-byte boundary */
    stack_top &= ~(uintptr_t)15;

    /*
     * Build the initial stack frame.
     * sp[-1] through sp[-8] are slots for 8 uint64_t values.
     */
    uint64_t *sp = (uint64_t *)stack_top;

    sp[-1] = (uint64_t)(uintptr_t)abort; /* sentinel: crash if func returns */
    sp[-2] = (uint64_t)(uintptr_t)func;  /* "return address" for ret        */
    sp[-3] = 0;                           /* rbp                             */
    sp[-4] = 0;                           /* rbx                             */
    sp[-5] = 0;                           /* r12                             */
    sp[-6] = 0;                           /* r13                             */
    sp[-7] = 0;                           /* r14                             */
    sp[-8] = 0;                           /* r15                             */

    ctx->sp = (void *)(sp - 8);
    ctx->error = 0;

    apth_debug("leave: stack_top=%p sp=%p func=%p",
               (void *)stack_top, ctx->sp, (void *)(uintptr_t)func);
    return true;
}
