#ifndef __LIBAPTH_HOOK_LIBC_HOOK_PROCESS_H
#define __LIBAPTH_HOOK_LIBC_HOOK_PROCESS_H

#include "hook_utils.h"
#include <sys/types.h>
#include <bits/types/struct_rusage.h>

#define APTH_LIST_OF_HOOK_PROCESS \
    X(system)                     \
    X(fork)                       \
    X(_Fork)                      \
    X(vfork)                      \
    X(waitpid)                    \
    X(wait)                       \
    X(wait4)                      \
    X(exit)

// ==================== 27.1 Running a Command ====================
// TODO: need to be rewritten
APTH_DECLARE_HOOK(int, system, const char *cmd)

// ==================== 27.4 Creating a Process ====================
// TODO: implementation
APTH_DECLARE_HOOK(pid_t, fork, void)

// TODO: implementation
APTH_DECLARE_HOOK(pid_t, _Fork, void)

// TODO: implementation
APTH_DECLARE_HOOK(pid_t, vfork, void)

// ==================== 27.7 Process Completion ====================
APTH_DECLARE_HOOK(pid_t, waitpid, pid_t wpid, int *status, int options)

// TODO: implementation
APTH_DECLARE_HOOK(pid_t, wait, int *status_ptr)
// TODO: implementation
APTH_DECLARE_HOOK(pid_t, wait4, pid_t pid, int *status_ptr, int options, struct rusage *usage)

// ==================== 26.7 Program Termination ====================
// TODO: implementation
APTH_DECLARE_FETCH_LIBCFUNC(void, exit, int status)

#endif // __LIBAPTH_HOOK_LIBC_HOOK_PROCESS_H
