#ifndef __LIBAPTH_INTERNAL_APTH_PREEMPT_H
#define __LIBAPTH_INTERNAL_APTH_PREEMPT_H

#include "utils/archplattoold.h"

// ===================== Preemption Modes =====================
//
// LIBAPTH supports three preemption modes, selected at compile time:
//
// 1. APTH_PREEMPT_NONE (default):
//    Pure cooperative scheduling. Threads must explicitly yield or
//    call a hooked I/O function to give up CPU. A CPU-bound thread
//    that never does I/O will starve all other threads on its
//    scheduler. Suitable for I/O-heavy servers or when used with
//    runtimes that have their own safepoints (e.g., JVM).
//
// 2. APTH_PREEMPT_SIGNAL:
//    Signal-based preemption. Each scheduler sets a per-worker timer
//    (SIGALRM or SIGPROF) that fires every N milliseconds. The signal
//    handler sets a flag; the next hooked function or cancellation
//    point checks the flag and yields if set. This provides fairness
//    for CPU-bound threads at the cost of one timer signal per
//    scheduler per quantum.
//
// 3. APTH_PREEMPT_INSTRUMENT:
//    Compiler instrumentation mode. Requires compiling user code with
//    -finstrument-functions (GCC/Clang). The __cyg_profile_func_enter
//    hook checks a preemption flag and yields if the thread has
//    exceeded its time quantum. Deterministic and signal-free, but
//    requires recompilation of user code.
//
// Usage:
//   gcc -DAPTH_PREEMPT_SIGNAL ...      # enable signal preemption
//   gcc -DAPTH_PREEMPT_INSTRUMENT ...  # enable instrumentation
//   gcc ...                            # default: cooperative only
//
// Note: APTH_PREEMPT_SIGNAL and APTH_PREEMPT_INSTRUMENT are mutually
// exclusive. Defining both is a compile-time error.

#if defined(APTH_PREEMPT_SIGNAL) && defined(APTH_PREEMPT_INSTRUMENT)
#error "APTH_PREEMPT_SIGNAL and APTH_PREEMPT_INSTRUMENT are mutually exclusive"
#endif

// Default preemption quantum in milliseconds
#ifndef APTH_PREEMPT_QUANTUM_MS
#define APTH_PREEMPT_QUANTUM_MS 10
#endif

// Signal used for preemption (must be unblocked in worker threads)
#ifdef APTH_PREEMPT_SIGNAL
#include <signal.h>
#define APTH_PREEMPT_SIGNO SIGPROF
#endif

// Initialize preemption system (called during library init)
APTH_INTERNAL void apth_preempt_init(void);

// Tear down preemption system (called during library drop)
APTH_INTERNAL void apth_preempt_drop(void);

// Start preemption timer for the current worker thread.
// Called when a scheduler is initialized and ready to dispatch.
APTH_INTERNAL void apth_preempt_arm(void);

// Stop preemption timer for the current worker thread.
// Called when a scheduler is shutting down.
APTH_INTERNAL void apth_preempt_disarm(void);

// Check if preemption is requested and yield if so.
// This is the inline fast-path check inserted at preemption points.
// For APTH_PREEMPT_SIGNAL: called from hooked functions.
// For APTH_PREEMPT_INSTRUMENT: called from __cyg_profile_func_enter.
// For APTH_PREEMPT_NONE: compiles to nothing.
APTH_INTERNAL void apth_preempt_check(void);

#endif // __LIBAPTH_INTERNAL_APTH_PREEMPT_H
