/*
 * apth_ctx_x86_64.S - x86-64 assembly context switch for LIBAPTH
 *
 * Replaces glibc swapcontext/makecontext which make two sigprocmask
 * syscalls per context switch.  This implementation saves/restores
 * only the callee-saved registers per the SysV ABI, costing ~20ns
 * instead of ~2000ns.
 *
 * Context layout on the stack (saved by apth_ctx_switch_asm):
 *   [rsp + 0x00]  r15
 *   [rsp + 0x08]  r14
 *   [rsp + 0x10]  r13
 *   [rsp + 0x18]  r12
 *   [rsp + 0x20]  rbx
 *   [rsp + 0x28]  rbp
 *   [rsp + 0x30]  return address (pushed by call or set up by ctx_set)
 *
 * The "context" is simply a saved stack pointer (void *sp).
 */

#if defined(__x86_64__) && defined(__linux__)

.text

/*
 * void apth_ctx_switch_asm(void **old_sp, void *new_sp);
 *
 * Save callee-saved registers on the current stack, store rsp into
 * *old_sp, restore rsp from new_sp, pop callee-saved registers, ret.
 *
 * Parameters (SysV ABI):
 *   rdi = void **old_sp   — where to store the old stack pointer
 *   rsi = void *new_sp    — stack pointer to restore
 */
.globl apth_ctx_switch_asm
.type  apth_ctx_switch_asm, @function
.align 16
apth_ctx_switch_asm:
    /* Save callee-saved registers onto the current stack */
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    /* Save current stack pointer to *old_sp */
    movq    %rsp, (%rdi)

    /* Restore new stack pointer */
    movq    %rsi, %rsp

    /* Pop callee-saved registers from the new stack */
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp

    /* Return to the saved return address on the new stack.
     * For a resumed context, this returns to the caller of
     * apth_ctx_switch_asm.  For a newly created context (set up
     * by apth_ctx_set), this jumps to the trampoline. */
    ret

.size apth_ctx_switch_asm, .-apth_ctx_switch_asm

#else
#error "apth_ctx_x86_64.S is only for x86-64 Linux"
#endif

/* Mark the stack as non-executable (security hardening) */
.section .note.GNU-stack,"",@progbits
