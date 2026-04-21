#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "apth_preempt.h"
#include "common.h"
#include "internal/types.h"
#include "internal/apth_sched.h"
#include "hook_libc/hooked_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/archplattoold.h"

// Preempt hook (defined in apth_safepoint.c)
extern void __apth_fire_preempt_hook(apth_t th);

// ===================== Cooperative (no preemption) =====================
#if !defined(APTH_PREEMPT_SIGNAL) && !defined(APTH_PREEMPT_INSTRUMENT)

APTH_INTERNAL void apth_preempt_init(void)  { /* nop */ }
APTH_INTERNAL void apth_preempt_drop(void)  { /* nop */ }
APTH_INTERNAL void apth_preempt_arm(void)   { /* nop */ }
APTH_INTERNAL void apth_preempt_disarm(void){ /* nop */ }
APTH_INTERNAL void apth_preempt_check(void) { /* nop */ }

// ===================== Signal-based preemption =====================
#elif defined(APTH_PREEMPT_SIGNAL)

#include <signal.h>
#include <time.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>

// Per-worker timer ID
static APTH_THREAD_LOCAL timer_t __preempt_timer;
static APTH_THREAD_LOCAL int __preempt_timer_armed = 0;

// Assembly context-switch bounds: don't preempt mid-switch.
extern void apth_ctx_switch_asm(void **old_sp, void *new_sp);
extern char __apth_ctx_switch_asm_end[];

// Assembly full-context restore (defined in apth_ctx_x86_64.S).
extern void __apth_preempt_restore(greg_t *gregs,
                                   struct _libc_fpstate *fpstate)
    __attribute__((noreturn));

// Forward declare the trampoline.
static void __apth_preempt_trampoline(void) __attribute__((noreturn, noinline));

/*
 * Signal handler for true preemptive scheduling.
 *
 * When the per-worker SIGPROF timer fires, this handler:
 *   1. Checks that an M:N thread is running and is preemptible.
 *   2. Saves the full interrupted register state (GP + FP) into the TCB.
 *   3. Modifies the signal return context to jump to a trampoline that
 *      cooperatively yields to the scheduler.
 *
 * After the yield, the trampoline calls __apth_preempt_restore() to
 * restore the full saved state, resuming the thread transparently.
 *
 * Async-signal-safe: only reads/writes TCB fields and ucontext values.
 */
static void apth_preempt_signal_handler(int sig, siginfo_t *info, void *uctx)
{
    (void)sig;
    (void)info;

    apth_t cur = CUR_APTH;
    if (cur == NULL)                    return;
    if (cur->is_dedicated)              return;
    if (cur->dispatch_prevented)        return;
    if (cur->was_preempted)             return;
    ucontext_t *uc = (ucontext_t *)uctx;
    greg_t rip = uc->uc_mcontext.gregs[REG_RIP];
    greg_t rsp = uc->uc_mcontext.gregs[REG_RSP];

    // Don't preempt inside the assembly context switch.
    if (rip >= (greg_t)apth_ctx_switch_asm &&
        rip <  (greg_t)__apth_ctx_switch_asm_end)
        return;

    // Don't preempt if RSP is outside the thread's stack.
    // Between SET_CUR_APTH(th) and ctx_switch (and after ctx_switch
    // returns until SET_CUR_APTH(NULL)), CUR_APTH is set but execution
    // is on the scheduler's stack.  Preempting here would corrupt the
    // scheduler's context and cause a crash on next dispatch.
    if (cur->stack_mem_start != NULL)
    {
        greg_t stack_lo = (greg_t)cur->stack_mem_start;
        greg_t stack_hi = stack_lo + (greg_t)cur->stacksize;
        if (rsp < stack_lo || rsp >= stack_hi)
            return;
    }

    // Save full GP register state from the interrupted context.
    memcpy(cur->preempt_gregs, uc->uc_mcontext.gregs,
           sizeof(cur->preempt_gregs));

    // Save FP/SSE state.  The kernel stored it at *uc_mcontext.fpregs.
    if (uc->uc_mcontext.fpregs != NULL)
        memcpy(&cur->preempt_fpstate, uc->uc_mcontext.fpregs,
               sizeof(cur->preempt_fpstate));

    cur->was_preempted = true;

    // Redirect execution to the trampoline, below the red zone.
    // x86-64 ABI: 128-byte red zone below RSP.
    greg_t orig_rsp = uc->uc_mcontext.gregs[REG_RSP];
    greg_t new_rsp  = (orig_rsp - 256) & ~(greg_t)0xF;  // 16-byte aligned
    new_rsp -= 8;  // SysV ABI: RSP % 16 == 8 at function entry

    uc->uc_mcontext.gregs[REG_RSP] = new_rsp;
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)__apth_preempt_trampoline;
}

/*
 * Trampoline: runs after sigreturn with the modified context.
 * Stack is below the original thread's red zone.
 *
 * 1. Cooperatively yields to the scheduler (pre_yield_hook fires to
 *    save JVM state, post_resume_hook fires on re-dispatch to restore it).
 * 2. After re-dispatch, restores the full saved register state via the
 *    assembly routine, jumping back to the exact interrupted instruction.
 */
static void __apth_preempt_trampoline(void)
{
    apth_t self = CUR_APTH;

    __apth_fire_preempt_hook(self);

    self->yield_reason = APTH_YIELD_REASON_TIMESLICE;
    apth_yield();

    // Re-dispatched.  Restore full interrupted context.
    self->was_preempted = false;
    __apth_preempt_restore(self->preempt_gregs, &self->preempt_fpstate);
    // unreachable
}

APTH_INTERNAL void apth_preempt_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = apth_preempt_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (apth_func_raw(sigaction)(APTH_PREEMPT_SIGNO, &sa, NULL) < 0)
        apth_debug("WARNING: failed to install preemption signal handler");
}

APTH_INTERNAL void apth_preempt_drop(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    apth_func_raw(sigaction)(APTH_PREEMPT_SIGNO, &sa, NULL);
}

APTH_INTERNAL void apth_preempt_arm(void)
{
    // Ensure the preemption signal is unblocked on this worker pthread.
    // The worker may have inherited a blocked mask from the parent thread
    // (e.g., the JVM blocks most signals on its threads).
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, APTH_PREEMPT_SIGNO);
    pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);

    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));

    // SIGEV_THREAD_ID: deliver signal to THIS worker thread only.
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo  = APTH_PREEMPT_SIGNO;
    sev._sigev_un._tid = (int)syscall(SYS_gettid);

    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &__preempt_timer) < 0)
    {
        apth_debug("WARNING: failed to create preemption timer");
        return;
    }

    struct itimerspec its;
    its.it_value.tv_sec     = APTH_PREEMPT_QUANTUM_MS / 1000;
    its.it_value.tv_nsec    = (APTH_PREEMPT_QUANTUM_MS % 1000) * 1000000L;
    its.it_interval = its.it_value;

    if (timer_settime(__preempt_timer, 0, &its, NULL) < 0)
    {
        apth_debug("WARNING: failed to arm preemption timer");
        timer_delete(__preempt_timer);
        return;
    }

    __preempt_timer_armed = 1;
    apth_debug("preemption timer armed (quantum=%dms)", APTH_PREEMPT_QUANTUM_MS);
}

APTH_INTERNAL void apth_preempt_disarm(void)
{
    if (__preempt_timer_armed)
    {
        timer_delete(__preempt_timer);
        __preempt_timer_armed = 0;
    }
}

// Legacy check — only used by instrumentation mode now.
APTH_INTERNAL void apth_preempt_check(void) { /* nop for signal mode */ }

// ===================== Compiler instrumentation =====================
#elif defined(APTH_PREEMPT_INSTRUMENT)

// Per-worker tick counter. Incremented at every function entry.
// When it exceeds the threshold, a yield is triggered.
static APTH_THREAD_LOCAL unsigned int __instrument_counter = 0;

// Threshold: yield after this many function entries.
// Tunable at compile time.
#ifndef APTH_PREEMPT_INSTRUMENT_THRESHOLD
#define APTH_PREEMPT_INSTRUMENT_THRESHOLD 10000
#endif

APTH_INTERNAL void apth_preempt_init(void) { /* nop */ }
APTH_INTERNAL void apth_preempt_drop(void) { /* nop */ }
APTH_INTERNAL void apth_preempt_arm(void)
{
    __instrument_counter = 0;
}
APTH_INTERNAL void apth_preempt_disarm(void)
{
    __instrument_counter = 0;
}

APTH_INTERNAL void apth_preempt_check(void)
{
    if (++__instrument_counter >= APTH_PREEMPT_INSTRUMENT_THRESHOLD)
    {
        __instrument_counter = 0;
        apth_t cur = CUR_APTH;
        if (cur != NULL)
        {
            __apth_fire_preempt_hook(cur);
            cur->yield_reason = APTH_YIELD_REASON_TIMESLICE;
            apth_yield();
        }
    }
}

// GCC/Clang instrument hooks.
// Compiled into user code when -finstrument-functions is passed.
// These are weak symbols so they don't conflict if the user has their own.
__attribute__((no_instrument_function))
void __cyg_profile_func_enter(void *this_fn, void *call_site)
{
    (void)this_fn;
    (void)call_site;
    apth_preempt_check();
}

__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void *this_fn, void *call_site)
{
    (void)this_fn;
    (void)call_site;
    // No action on exit — only check on entry
}

#endif // preemption mode selection
