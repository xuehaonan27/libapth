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

// Per-worker preemption flag. Set by signal handler, cleared by check.
// Using _Thread_local so each worker pthread has its own flag.
static APTH_THREAD_LOCAL volatile sig_atomic_t __preempt_pending = 0;

// Per-worker timer ID
static APTH_THREAD_LOCAL timer_t __preempt_timer;
static APTH_THREAD_LOCAL int __preempt_timer_armed = 0;

// Signal used for preemption timer. SIGPROF is a good choice because:
// - It's per-thread (timer_create with SIGEV_THREAD_ID)
// - It counts both user and system CPU time
// - It's rarely used by applications
#define APTH_PREEMPT_SIGNO SIGPROF

/*
 * Signal handler for preemption.
 *
 * With the assembly context switch (no sigprocmask in the switch path),
 * this signal can fire at any point during user thread execution.  The
 * handler only sets a flag; the actual yield happens at the next
 * preemption check point (see apth_preempt_check).
 *
 * Preemption check points:
 *   1. Every hooked I/O function (read, write, connect, accept, etc.)
 *   2. Every synchronization operation (mutex lock/unlock, cond wait, etc.)
 *   3. The scheduler loop before dispatching a new thread
 *   4. apth_yield() and apth_yield_optional()
 *
 * Limitation: a pure CPU-bound loop that calls no hooked functions
 * will not be preempted until it makes a function call that includes
 * a preemption check.  For such workloads, use APTH_PREEMPT_INSTRUMENT
 * mode which checks at every function entry.
 */
static void apth_preempt_signal_handler(int sig, siginfo_t *info, void *uctx)
{
    (void)sig;
    (void)info;
    (void)uctx;
    __preempt_pending = 1;
}

APTH_INTERNAL void apth_preempt_init(void)
{
    // Install the preemption signal handler. We use SA_RESTART to avoid
    // disrupting blocking syscalls in the application.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = apth_preempt_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (apth_func_raw(sigaction)(APTH_PREEMPT_SIGNO, &sa, NULL) < 0)
    {
        apth_debug("WARNING: failed to install preemption signal handler");
    }
}

APTH_INTERNAL void apth_preempt_drop(void)
{
    // Restore default handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    apth_func_raw(sigaction)(APTH_PREEMPT_SIGNO, &sa, NULL);
}

APTH_INTERNAL void apth_preempt_arm(void)
{
    // Create a per-thread POSIX timer that delivers APTH_PREEMPT_SIGNO
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = APTH_PREEMPT_SIGNO;

    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &__preempt_timer) < 0)
    {
        apth_debug("WARNING: failed to create preemption timer");
        return;
    }

    // Arm the timer with the configured quantum
    struct itimerspec its;
    its.it_value.tv_sec = APTH_PREEMPT_QUANTUM_MS / 1000;
    its.it_value.tv_nsec = (APTH_PREEMPT_QUANTUM_MS % 1000) * 1000000L;
    its.it_interval = its.it_value; // Repeating

    if (timer_settime(__preempt_timer, 0, &its, NULL) < 0)
    {
        apth_debug("WARNING: failed to arm preemption timer");
        timer_delete(__preempt_timer);
        return;
    }

    __preempt_timer_armed = 1;
    __preempt_pending = 0;
    apth_debug("preemption timer armed (quantum=%dms)", APTH_PREEMPT_QUANTUM_MS);
}

APTH_INTERNAL void apth_preempt_disarm(void)
{
    if (__preempt_timer_armed)
    {
        timer_delete(__preempt_timer);
        __preempt_timer_armed = 0;
        __preempt_pending = 0;
    }
}

APTH_INTERNAL void apth_preempt_check(void)
{
    if (__preempt_pending)
    {
        __preempt_pending = 0;
        apth_t cur = CUR_APTH;
        if (cur != NULL)
        {
            __apth_fire_preempt_hook(cur);
            // Only yield if this is an actual apth, not scheduler context
            cur->yield_reason = APTH_YIELD_REASON_TIMESLICE;
            apth_yield();
        }
    }
}

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
