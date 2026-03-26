#ifndef __LIBAPTH_INTERNAL_APTH_CTX_H
#define __LIBAPTH_INTERNAL_APTH_CTX_H

#include "utils/archplattoold.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Lightweight thread context for x86-64.
 *
 * Instead of the glibc ucontext_t (~936 bytes, two sigprocmask syscalls
 * per swapcontext), we store only a saved stack pointer (8 bytes) and
 * the errno value (4 bytes).  Callee-saved registers are saved on the
 * stack by the assembly context-switch routine.
 *
 * Total size: 16 bytes (vs ~940 bytes for the old ucontext-based context).
 */
struct apth_cxt_st
{
    void *sp;   /* saved stack pointer */
    int error;  /* saved errno */
};
typedef struct apth_cxt_st *apth_cxt_t;

/* Assembly routine: save callee-saved regs to current stack, store rsp
 * to *old_sp, restore rsp from new_sp, pop regs, ret.  Defined in
 * apth_ctx_x86_64.S. */
extern void apth_ctx_switch_asm(void **old_sp, void *new_sp);

/* Switch from old context to new context.  Saves/restores errno. */
APTH_INTERNAL void apth_ctx_switch(apth_cxt_t old, apth_cxt_t new);

/* Initialize a context for a new thread.  func will be called (with no
 * arguments) when the context is first switched to.  stack_mem_start
 * points to the beginning of the usable stack area; stacksize is its
 * size in bytes. */
APTH_INTERNAL bool apth_ctx_set(apth_cxt_t ctx, void (*func)(void),
                                char *stack_mem_start, size_t stacksize);

#endif /* __LIBAPTH_INTERNAL_APTH_CTX_H */
