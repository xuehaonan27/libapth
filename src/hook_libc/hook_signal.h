#ifndef __LIBAPTH_HOOK_LIBC_HOOK_SIGNAL_H
#define __LIBAPTH_HOOK_LIBC_HOOK_SIGNAL_H

#define _POSIX_C_SOURCE 200809L // For struct sigaction
#include <signal.h>
#include "hook_utils.h"
#include <bits/types/stack_t.h>
#include <bits/types/struct_sigstack.h>

#define APTH_LIST_OF_HOOK_SIGNAL \
    X(signal)                    \
    X(sysv_signal)               \
    X(__sysv_signal)             \
    X(sigaction)                 \
    X(sigwait)                   \
    X(raise)                     \
    X(gsignal)                   \
    X(sigprocmask)               \
    X(sigpending)                \
    X(pause)                     \
    X(sigsuspend)                \
    X(sigaltstack)               \
    X(sigstack)

// ==================== 25.3 Specifying Signal Actions ====================
typedef void (*sighandler_t)(int);
APTH_DECLARE_HOOK(sighandler_t, signal, int sig, sighandler_t handler)
APTH_DECLARE_HOOK(sighandler_t, sysv_signal, int sig, sighandler_t handler)
APTH_DECLARE_HOOK(sighandler_t, __sysv_signal, int sig, sighandler_t handler)
APTH_DECLARE_HOOK(int, sigaction, int signum, const struct sigaction *restrict act,
                  struct sigaction *restrict oldact)
APTH_DECLARE_HOOK(int, sigwait, const sigset_t *set, int *sigp)

// ==================== 25.6 Generating Signals ====================
APTH_DECLARE_HOOK(int, raise, int sig)
APTH_DECLARE_HOOK(int, gsignal, int signum)
// Actually these is really nothing to hook about `kill` since we only care how
// signals should be handled within our process, not others.
// And even if the process called `kill (getpid (), sig)`, sending signal to
// ourself, then the global signal handlers should catch the signal and dispatch
// it to a certain apth.
// APTH_DECLARE_HOOK(int, kill, pid_t pid, int signum)

// ==================== 25.7 Blocking Signals ====================
// GLIBC says "Note that you must not use sigprocmask in multi-threaded processes".
// Since `sigprocmask` behaviour is not specified, we could just redirect it to `apth_sigmask`
APTH_DECLARE_HOOK(int, sigprocmask, int how, const sigset_t *restrict set,
                  sigset_t *restrict oldset)
APTH_DECLARE_HOOK(int, sigpending, sigset_t *set)

// ==================== 25.8 Waiting for a Signal ====================
// TODO: implementation
APTH_DECLARE_HOOK(int, pause, void)

APTH_DECLARE_HOOK(int, sigsuspend, const sigset_t *mask)

// ==================== 25.9 Using a Separate Signal Stack ====================
// Note that currently we cannot actually use a separate signal stack, due to
// how LIBAPTH is implemented. See comment of `__apth_inject_signal_handler`
// in `src/internal/apth_signal.c` for more information.

APTH_DECLARE_HOOK(int, sigaltstack, const stack_t *restrict ss, stack_t *restrict oss)

// TODO: implementation
APTH_DECLARE_HOOK(int, sigstack, struct sigstack *stack, struct sigstack *oldstack)

// ==================== 25.10 BSD Signal Handling ====================
// Currently we only consider Linux, so BSD Unix is not in our consideration for now.

#endif // __LIBAPTH_HOOK_LIBC_HOOK_SIGNAL_H
