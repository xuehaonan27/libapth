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

/* Sentinel label — the signal handler checks whether RIP is inside
 * apth_ctx_switch_asm to avoid preempting mid-context-switch. */
.globl __apth_ctx_switch_asm_end
.type  __apth_ctx_switch_asm_end, @notype
__apth_ctx_switch_asm_end:

/*
 * void __apth_preempt_restore(greg_t *gregs, struct _libc_fpstate *fpstate)
 *
 * Restore the full register state that was saved by the preemption
 * signal handler and jump to the saved RIP.  Never returns.
 *
 * gregs layout (gregset_t indices, each 8 bytes):
 *   [0]=R8  [1]=R9  [2]=R10  [3]=R11  [4]=R12  [5]=R13
 *   [6]=R14 [7]=R15 [8]=RDI  [9]=RSI  [10]=RBP [11]=RBX
 *   [12]=RDX [13]=RAX [14]=RCX [15]=RSP [16]=RIP [17]=EFL
 *
 * Strategy: store RIP and EFLAGS below saved_RSP (in the red zone,
 * same approach as glibc setcontext), restore all GP registers, switch
 * stack, popfq, ret.
 */
.globl __apth_preempt_restore
.type  __apth_preempt_restore, @function
.align 16
__apth_preempt_restore:
    /* rdi = gregs,  rsi = fpstate (16-byte aligned) */

    /* 1. Restore FP/SSE state */
    fxrstor (%rsi)

    /* 2. Pre-stage RIP and EFLAGS below the saved RSP.
     *    saved_RSP-8  = saved RIP  (popped by final ret)
     *    saved_RSP-16 = saved EFLAGS (popped by popfq)  */
    movq    120(%rdi), %rax     /* saved RSP */
    movq    128(%rdi), %rcx     /* saved RIP */
    movq    %rcx, -8(%rax)
    movq    136(%rdi), %rcx     /* saved EFLAGS */
    movq    %rcx, -16(%rax)

    /* 3. Restore all 16 GP registers */
    movq      0(%rdi), %r8
    movq      8(%rdi), %r9
    movq     16(%rdi), %r10
    movq     24(%rdi), %r11
    movq     32(%rdi), %r12
    movq     40(%rdi), %r13
    movq     48(%rdi), %r14
    movq     56(%rdi), %r15
    movq     72(%rdi), %rsi
    movq     80(%rdi), %rbp
    movq     88(%rdi), %rbx
    movq     96(%rdi), %rdx
    movq    104(%rdi), %rax
    movq    112(%rdi), %rcx

    /* 4. Switch to saved stack (leaq doesn't touch EFLAGS) */
    movq    120(%rdi), %rsp
    leaq    -16(%rsp), %rsp     /* RSP = saved_RSP - 16 */

    /* 5. Restore RDI last */
    movq     64(%rdi), %rdi

    /* 6. Restore EFLAGS and jump to saved RIP */
    popfq                       /* RSP -> saved_RSP - 8 */
    ret                         /* RSP -> saved_RSP     */

.size __apth_preempt_restore, .-__apth_preempt_restore

#else
#error "apth_ctx_x86_64.S is only for x86-64 Linux"
#endif

/* Mark the stack as non-executable (security hardening) */
.section .note.GNU-stack,"",@progbits
