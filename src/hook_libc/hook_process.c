// #define _POSIX_C_SOURCE 200809L // For struct sigaction
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <signal.h>

#include "hook_process.h"
#include "hook_pthread.h"
#include "common.h" // For APTH_PATH_BINSH
#include "internal/apth_event.h"
#include "internal/types.h"
#include "internal/apth_sched.h"
#include "utils/atomic_wrapper.h"
#include <sys/stat.h> // For stat
#include <sys/wait.h> // For wait flags

// APTH variant of system(3)
APTH_DEFINE_HOOK(int, system, (const char *cmd), (cmd))
{
    struct sigaction sa_ign, sa_int, sa_quit;
    sigset_t ss_block, ss_old;
    struct stat sb;
    pid_t pid;
    int pstat;

    // POSIX calling convention: determine whether the Bourne Shell ("sh") is
    // available on this platform
    if (cmd == NULL)
    {
        if (stat(APTH_PATH_BINSH, &sb) == -1)
            return 0;
        return 1;
    }

    // Temporarily ignore SIGINT and SIGQUIT actions
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    sa_ign.sa_flags = 0;
    sigaction(SIGINT, &sa_ign, &sa_int);
    sigaction(SIGQUIT, &sa_ign, &sa_quit);

    // Block SIGCHLD signal
    sigemptyset(&ss_block);
    sigaddset(&ss_block, SIGCHLD);
    apth_func_raw(pthread_sigmask)(SIG_BLOCK, &ss_block, &ss_old);

    // Fork the current process
    pstat = -1;
    // Here we use hooked version of fork syscall to improve speed
    switch (pid = apth_func(fork)())
    {
    case -1: // Error
        break;
    case 0: // Child
        // Restore original signal dispositions and execute the command
        sigaction(SIGINT, &sa_int, NULL);
        sigaction(SIGQUIT, &sa_quit, NULL);
        apth_func_raw(pthread_sigmask)(SIG_SETMASK, &ss_old, NULL);

        // Stop the APTH scheduling
        apth_scheduler_kill();

        // Execute the command through Bourne Shell
        execl(APTH_PATH_BINSH, "sh", "-c", cmd, (char *)NULL);

        // POSIX compliant return in case execution failed
        apth_func_raw(exit)(127);
        break;
    default: // Parent
        // Wait until child process terminates
        // Here we use hooked version of waitpid to improve performance
        pid = apth_func(waitpid)(pid, &pstat, 0);
        break;
    }

    // Restore original signal dispositions
    sigaction(SIGINT, &sa_int, NULL);
    sigaction(SIGQUIT, &sa_quit, NULL);
    apth_func_raw(pthread_sigmask)(SIG_SETMASK, &ss_old, NULL);

    // Return error or child process result code
    return (pid == -1 ? -1 : pstat);
}

APTH_DEFINE_HOOK(pid_t, fork, (void), ())
{
    apth_hook_debug(fork);

    // fork() in a userspace threading library is complex
    // After fork(), only the calling thread exists in the child
    // All other threads disappear, which can cause issues with locks, etc.

    // For now, we call the raw fork and handle the child process
    pid_t pid = apth_func_raw(fork)();

    if (pid == 0)
    {
        // Child process: stop the APTH scheduler
        // Only the current apth exists in the child
        apth_scheduler_kill();
    }

    return pid;
}

APTH_DEFINE_HOOK(pid_t, _Fork, (void), ())
{
    apth_hook_debug(_Fork);

    // _Fork() is similar to fork() but doesn't run atfork handlers
    // For APTH, we implement it the same way as fork()
    pid_t pid = apth_func_raw(fork)();

    if (pid == 0)
    {
        // Child process: stop the APTH scheduler
        apth_scheduler_kill();
    }

    return pid;
}

APTH_DEFINE_HOOK(pid_t, vfork, (void), ())
{
    apth_hook_debug(vfork);

    // vfork() is like fork() but the parent is suspended until child calls exec or exit
    // The child shares the parent's address space
    // This is dangerous with userspace threads, so we just use fork()

    pid_t pid = apth_func_raw(vfork)();

    if (pid == 0)
    {
        // Child process: stop the APTH scheduler
        apth_scheduler_kill();
    }

    return pid;
}

// APTH variant of waitpid(2)
APTH_DEFINE_HOOK(pid_t, waitpid,
                 (pid_t wpid, int *status, int options),
                 (wpid, status, options))
{
    pid_t pid;
    apth_t cur = CUR_APTH;

    apth_debug("apth_waitpid: called from thread \"%s\"", cur->name);
    for (;;)
    {
        // Do a non-blocking poll for the pid using raw LIBC call
        while ((pid = apth_func_raw(waitpid)(wpid, status, options | WNOHANG)) < 0 && errno == EINTR)
            ;

        // If pid was found or caller requested a polling return immediately
        if (pid == -1 || pid > 0 || (pid == 0 && (options & WNOHANG)))
            break;

        // Else wait a little bit
        struct apth_event_st ev = EVENT_TIME(apth_timeout(0, 250000));
        apth_wait_event(&ev);
    }

    apth_debug("apth_waitpid: leave to thread \"%s\"", cur->name);
    return pid;
}

APTH_DEFINE_HOOK(pid_t, wait, (int *status_ptr), (status_ptr))
{
    apth_hook_debug(wait);

    // wait() is equivalent to waitpid(-1, status_ptr, 0)
    // Wait for any child process
    return apth_func(waitpid)(-1, status_ptr, 0);
}

APTH_DEFINE_HOOK(pid_t, wait4,
                 (pid_t pid, int *status_ptr, int options, struct rusage *usage),
                 (pid, status_ptr, options, usage))
{
    apth_hook_debug(wait4);
    pid_t result;
    apth_t cur = CUR_APTH;

    apth_debug("apth_wait4: called from thread \"%s\"", cur->name);
    for (;;)
    {
        // Do a non-blocking poll using raw LIBC call
        while ((result = apth_func_raw(wait4)(pid, status_ptr, options | WNOHANG, usage)) < 0 && errno == EINTR)
            ;

        // If pid was found or caller requested a polling return immediately
        if (result == -1 || result > 0 || (result == 0 && (options & WNOHANG)))
            break;

        // Else wait a little bit
        struct apth_event_st ev = EVENT_TIME(apth_timeout(0, 250000));
        apth_wait_event(&ev);
    }

    apth_debug("apth_wait4: leave to thread \"%s\"", cur->name);
    return result;
}

APTH_FETCH_LIBCFUNC(exit)
