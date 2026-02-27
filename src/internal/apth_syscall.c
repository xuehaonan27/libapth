// #include "common.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/epoll.h>
// #include <sys/types.h>
#include <stdarg.h>
#include <malloc.h> // for malloc
// #include <sys/uio.h>
#include <bits/uio_lim.h> // for __IOV_MAX
#ifdef __IOV_MAX
#define UIO_MAXIOV __IOV_MAX
#else
#undef UIO_MAXIOV
#endif

#define APTH_LIST_OF_FETCH_ONLY    \
    X(pthread_create)              \
    X(pthread_sigmask)             \
    X(pthread_self)                \
    X(pthread_kill)                \
    X(pthread_key_create)          \
    X(pthread_getspecific)         \
    X(pthread_setspecific)         \
    X(pthread_attr_init)           \
    X(pthread_join)                \
    X(pthread_exit)                \
    X(pthread_attr_setdetachstate) \
    X(pthread_attr_setaffinity_np) \
    X(pthread_cancel)              \
    X(exit)                        \
    X(pipe)

#define APTH_LIST_OF_SYSCALLS \
    X(nanosleep)              \
    X(usleep)                 \
    X(sleep)                  \
    X(sigwait)                \
    X(sigaction)              \
    X(signal)                 \
    X(__sysv_signal)          \
    X(bsd_signal)             \
    X(sigpending)             \
    X(sigprocmask)            \
    X(sigsuspend)             \
    X(raise)                  \
    X(sigaltstack)            \
    X(waitpid)                \
    X(fork)                   \
    X(system)                 \
    X(select)                 \
    X(pselect)                \
    X(socket)                 \
    X(connect)                \
    X(close)                  \
    X(accept)                 \
    X(read)                   \
    X(write)                  \
    X(readv)                  \
    X(writev)                 \
    X(sendto)                 \
    X(recvfrom)               \
    X(send)                   \
    X(recv)                   \
    X(poll)                   \
    X(setsockopt)             \
    X(setenv)                 \
    X(unsetenv)               \
    X(getenv)

#define X APTH_FETCH_LIBCFUNC
APTH_LIST_OF_FETCH_ONLY
#undef X

APTH_INTERNAL int apth_syscall_system_init(void)
{
    // Since before syscall is loaded and hooked, `write` does not exists,
    // so use stdio for debugging.
    // FIX: now apth_debug could auto-detect and pick right way to output
    apth_debug("enter");
#define X(name)                                                             \
    if (apth_syscall_init(name)() != 0)                                     \
    {                                                                       \
        apth_debug("fail to initialize system call `%s`", stringify(name)); \
        return -1;                                                          \
    }
    APTH_LIST_OF_SYSCALLS
    APTH_LIST_OF_FETCH_ONLY
#undef X
    apth_debug("leave");
    return 0;
}

APTH_INTERNAL int apth_syscall_system_drop(void)
{
    // TODO("apth_syscall_system_drop");
    apth_debug("enter");
    apth_debug("leave");
    return 0;
}

// APTH variant of nanosleep(2)
APTH_DEFINE_SYSCALL(
    int, nanosleep,
    (const struct timespec *rqtp, struct timespec *rmtp),
    (rqtp, rmtp))
{
    apth_time_t until;
    apth_time_t offset;
    apth_time_t now;
    apth_event_t ev;

    // Consistency checks for POSIX conformance
    if (rqtp == NULL)
        return apth_error(-1, EFAULT);
    if (rqtp->tv_nsec < 0 || rqtp->tv_nsec > (1000 * 1000000))
        return apth_error(-1, EINVAL);

    // Short-circuit
    if (rqtp->tv_sec == 0 && rqtp->tv_nsec == 0)
        return 0;

    // Calculate asleep time
    offset = apth_time((long)(rqtp->tv_sec), (long)(rqtp->tv_nsec) / 1000);
    apth_time_set(&until, APTH_TIME_NOW);
    apth_time_add(&until, &offset);

    // And let apth sleeps until this time is elapsed
    if ((ev = apth_event_time(APTH_EVENT_MODE_STATIC, until)) == APTH_EVENT_NULL)
        return apth_error(-1, errno);
    apth_wait_event(ev);

    // Optionally provide amount of slept time
    if (rmtp != NULL)
    {
        apth_time_set(&now, APTH_TIME_NOW);
        apth_time_sub(&until, &now);
        rmtp->tv_sec = until.tv_sec;
        rmtp->tv_nsec = until.tv_usec * 1000;
    }

    return 0;
}

// APTH variant of usleep(3)
APTH_DEFINE_SYSCALL(int, usleep, (unsigned int usec), (usec))
{
    apth_time_t until;
    apth_time_t offset;
    apth_event_t ev;

    // Short-circuit
    if (usec == 0)
        return 0;

    // Calculate asleep time
    offset = apth_time((long)(usec / 1000000), (long)(usec % 1000000));
    apth_time_set(&until, APTH_TIME_NOW);
    apth_time_add(&until, &offset);

    /* and let thread sleep until this time is elapsed */
    if ((ev = apth_event_time(APTH_EVENT_MODE_STATIC, until)) == NULL)
        return apth_error(-1, errno);
    apth_wait_event(ev);

    return 0;
}

APTH_DEFINE_SYSCALL(unsigned int, sleep, (unsigned int sec), (sec))
{
    apth_time_t until;
    apth_time_t offset;
    apth_event_t ev;

    // Consistency check
    if (sec == 0)
        return 0;

    // Calculate asleep time
    offset = apth_time(sec, 0);
    apth_time_set(&until, APTH_TIME_NOW);
    apth_time_add(&until, &offset);

    // And let thread sleep until this time is elapsed
    if ((ev = apth_event_time(APTH_EVENT_MODE_STATIC, until)) == NULL)
        return sec;
    apth_wait_event(ev);

    return 0;
}

// APTH variant of POSIX sigwait(3)
APTH_DEFINE_SYSCALL(int, sigwait, (const sigset_t *set, int *sigp), (set, sigp))
{
    apth_hook_debug(sigwait);

    apth_t self = cur_apth();
    if (set == NULL || sigp == NULL)
        return apth_error(EINVAL, EINVAL);

    // Check whether there's signal already pending
    lll_lock(&self->siglock, "sigwait");
    for (int sig = 1; sig < APTH_NSIG; sig++)
    {
        if (sigismember(set, sig) && sigismember(&self->sigpending, sig))
        {
            sigdelset(&self->sigpending, sig);
            self->sigpendcnt--;
            lll_unlock(&self->siglock, "sigwait");
            *sigp = sig;
            return 0;
        }
    }
    lll_unlock(&self->siglock, "sigwait");

    // No signal is pending, then we should wait
    apth_event_t ev = apth_event_sigs(APTH_EVENT_MODE_STATIC, set, sigp);
    if (ev == NULL)
        return apth_error(errno, errno);
    apth_wait_event(ev);

    // When the event is marked as OCCURRED by event manager,
    // *sigp should have been set to proper value.
    apth_event_free(ev);
    return 0;
}

// For signals like SIGKILL, SIGSTOP, SIGSEGV, SIGBUS, SIGFPE, SIGILL, etc,
// generated by hardware or could not be ignored, we must register the
// sigaction to kernel level (via `apth_syscall_raw(sigaction)`) as well, since
// they are generated by hardware and could not be simulated by software.
// For these signals, `libapth` will register a trampoline handler in kernel,
// and set the pending bit of the signal in triggering apth, and then return from
// then handler, instead of directly execute user registered handler in kernel
// context.
APTH_DEFINE_SYSCALL(int, sigaction,
                    (int sig, const struct sigaction *restrict act,
                     struct sigaction *restrict oldact),
                    (sig, act, oldact))
{
    apth_hook_debug(sigaction);

    // Reject invalid signal number
    if (sig <= 0 || sig >= APTH_NSIG || sig == SIGKILL || sig == SIGSTOP)
        return apth_error(-1, EINVAL);

    lll_lock(&APTH_GLOBAL_SIGACTIONS.lock, "sigaction");

    // Return old action
    if (oldact != NULL)
        *oldact = APTH_GLOBAL_SIGACTIONS.actions[sig];

    // Set new action
    if (act != NULL)
        APTH_GLOBAL_SIGACTIONS.actions[sig] = *act;

    lll_unlock(&APTH_GLOBAL_SIGACTIONS.lock, "sigaction");
    return 0;
}

APTH_DEFINE_SYSCALL(sighandler_t, signal, (int sig, sighandler_t handler), (sig, handler))
{
    apth_hook_debug(signal);

    // Reject invalid signal number
    if (sig <= 0 || sig >= APTH_NSIG || sig == SIGKILL || sig == SIGSTOP)
        return apth_error(SIG_ERR, EINVAL);

    sighandler_t prev;

    lll_lock(&APTH_GLOBAL_SIGACTIONS.lock, "sigaction");

    // Return old action
    prev = APTH_GLOBAL_SIGACTIONS.actions[sig].sa_handler;

    // Set new action
    APTH_GLOBAL_SIGACTIONS.actions[sig].sa_handler = handler;
    sigemptyset(&APTH_GLOBAL_SIGACTIONS.actions[sig].sa_mask);
    APTH_GLOBAL_SIGACTIONS.actions[sig].sa_flags = SA_RESTART;

    lll_unlock(&APTH_GLOBAL_SIGACTIONS.lock, "sigaction");
    return prev;
}

APTH_DEFINE_SYSCALL(sighandler_t, __sysv_signal, (int sig, sighandler_t handler), (sig, handler))
{
    if (sig <= 0 || sig >= APTH_NSIG || sig == SIGKILL || sig == SIGSTOP)
        return apth_error(SIG_ERR, EINVAL);
    sighandler_t prev;
    lll_lock(&APTH_GLOBAL_SIGACTIONS.lock, "__sysv_signal");
    prev = APTH_GLOBAL_SIGACTIONS.actions[sig].sa_handler;
    APTH_GLOBAL_SIGACTIONS.actions[sig].sa_handler = handler;
    sigemptyset(&APTH_GLOBAL_SIGACTIONS.actions[sig].sa_mask);
    APTH_GLOBAL_SIGACTIONS.actions[sig].sa_flags = SA_RESTART;
    lll_unlock(&APTH_GLOBAL_SIGACTIONS.lock, "__sysv_signal");
    return prev;
}

APTH_DEFINE_SYSCALL(sighandler_t, bsd_signal, (int sig, sighandler_t handler), (sig, handler))
{
    if (sig <= 0 || sig >= APTH_NSIG || sig == SIGKILL || sig == SIGSTOP)
        return apth_error(SIG_ERR, EINVAL);
    sighandler_t prev;
    lll_lock(&APTH_GLOBAL_SIGACTIONS.lock, "bsd_signal");
    prev = APTH_GLOBAL_SIGACTIONS.actions[sig].sa_handler;
    APTH_GLOBAL_SIGACTIONS.actions[sig].sa_handler = handler;
    sigemptyset(&APTH_GLOBAL_SIGACTIONS.actions[sig].sa_mask);
    APTH_GLOBAL_SIGACTIONS.actions[sig].sa_flags = SA_RESTART | SA_NODEFER;
    lll_unlock(&APTH_GLOBAL_SIGACTIONS.lock, "bsd_signal");
    return prev;
}

APTH_DEFINE_SYSCALL(int, sigprocmask,
                    (int how, const sigset_t *restrict set, sigset_t *restrict oldset),
                    (how, set, oldset))
{
    // Manual of sigprocmask(2) says:
    //   sigprocmask()  is used to fetch and/or change the signal mask of the calling thread.
    // The signal mask is the set of signals whose delivery is currently blocked for the
    // caller (see also signal(7) for more details).
    //
    // Manual also says:
    //   The use of sigprocmask() is unspecified in a multithreaded process;
    // see pthread_sigmask(3).
    //
    // Since `sigprocmask` behaviour is not specified, we could just redirect it to `apth_sigmask`
    return apth_sigmask(how, set, oldset);
}

APTH_DEFINE_SYSCALL(int, sigpending, (sigset_t * set), (set))
{
    if (set == NULL)
        return apth_error(-1, EFAULT);
    apth_t cur = cur_apth();
    if (APTH_IS_FAKE_SCHED(cur))
        return apth_syscall_raw(sigpending)(set);

    lll_lock(&cur->siglock, "sigpending");
    *set = cur->sigpending;
    lll_unlock(&cur->siglock, "sigpending");
    return 0;
}

// Event manager will check the signal
static bool __apth_sigsuspend_check(void *arg)
{
    apth_t th = (apth_t)arg;
    lll_lock(&th->siglock, "__apth_sigsuspend_check");
    bool found = false;
    for (int sig = 1; sig < APTH_NSIG; sig++)
    {
        if (sigismember(&th->sigpending, sig) && !sigismember(&th->sigmask, sig))
        {
            found = true;
            break;
        }
    }
    lll_unlock(&th->siglock, "__apth_sigsuspend_check");
    return found;
}

APTH_DEFINE_SYSCALL(int, sigsuspend, (const sigset_t *mask), (mask))
{
    apth_t self = cur_apth();

    // Replace signal mask temporarily
    sigset_t oldmask = self->sigmask;
    self->sigmask = *mask;

    // Wait for a non-blocked signal delivered
    apth_event_t ev = apth_event_func(
        APTH_EVENT_MODE_STATIC,
        __apth_sigsuspend_check,
        self,
        apth_time(0, 50000));
    apth_wait_event(ev);

    // Restore signal mask
    self->sigmask = oldmask;

    // Deliver pending signal to apth self
    apth_deliver_pending_signals(self);

    return apth_error(-1, EINTR);
}

APTH_DEFINE_SYSCALL(int, raise, (int sig), (sig))
{
    apth_t self = cur_apth();
    if (APTH_IS_FAKE_SCHED(self))
        return apth_syscall_raw(raise)(sig);
    return apth_kill(self, sig);
}

APTH_DEFINE_SYSCALL(int, sigaltstack, (const stack_t *restrict ss, stack_t *restrict oss), (ss, oss))
{
    apth_t cur = cur_apth();
    if (oss != NULL)
    {
        if (cur->sigaltstack_set)
            *oss = cur->signalstack;
        else
        {
            oss->ss_sp = NULL;
            oss->ss_size = 0;
            oss->ss_flags = SS_DISABLE;
        }
    }
    if (ss != NULL)
    {
        if (ss->ss_flags & SS_DISABLE)
            cur->sigaltstack_set = false;
        else
        {
            cur->signalstack = *ss;
            cur->sigaltstack_set = true;
        }
    }
    return 0;
}

// APTH variant of waitpid(2)
APTH_DEFINE_SYSCALL(pid_t, waitpid,
                    (pid_t wpid, int *status, int options),
                    (wpid, status, options))
{
    apth_event_t ev;
    pid_t pid;
    apth_t cur = cur_apth();

    apth_debug("apth_waitpid: called from thread \"%s\"", cur->name);
    for (;;)
    {
        // Do a non-blocking poll for the pid using raw LIBC call
        while ((pid = apth_syscall_raw(waitpid)(wpid, status, options | WNOHANG)) < 0 && errno == EINTR)
            ;

        // If pid was found or caller requested a polling return immediately
        if (pid == -1 || pid > 0 || (pid == 0 && (options & WNOHANG)))
            break;

        // Else wait a little bit
        ev = apth_event_time(APTH_EVENT_MODE_STATIC, apth_timeout(0, 250000));
        apth_wait_event(ev);
    }

    apth_debug("apth_waitpid: leave to thread \"%s\"", cur->name);
    return pid;
}

APTH_DEFINE_SYSCALL(pid_t, fork, (void), ())
{
    TODO("fork");
}

// APTH variant of system(3)
APTH_DEFINE_SYSCALL(int, system, (const char *cmd), (cmd))
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
    apth_syscall_raw(pthread_sigmask)(SIG_BLOCK, &ss_block, &ss_old);

    // Fork the current process
    pstat = -1;
    // Here we use hooked version of fork syscall to improve speed
    switch (pid = apth_syscall(fork)())
    {
    case -1: // Error
        break;
    case 0: // Child
        // Restore original signal dispositions and execute the command
        sigaction(SIGINT, &sa_int, NULL);
        sigaction(SIGQUIT, &sa_quit, NULL);
        apth_syscall_raw(pthread_sigmask)(SIG_SETMASK, &ss_old, NULL);

        // Stop the APTH scheduling
        apth_scheduler_kill();

        // Execute the command through Bourne Shell
        execl(APTH_PATH_BINSH, "sh", "-c", cmd, (char *)NULL);

        // POSIX compliant return in case execution failed
        apth_syscall_raw(exit)(127);
        break;
    default: // Parent
        // Wait until child process terminates
        // Here we use hooked version of waitpid to improve performance
        pid = apth_syscall(waitpid)(pid, &pstat, 0);
        break;
    }

    // Restore original signal dispositions
    sigaction(SIGINT, &sa_int, NULL);
    sigaction(SIGQUIT, &sa_quit, NULL);
    apth_syscall_raw(pthread_sigmask)(SIG_SETMASK, &ss_old, NULL);

    // Return error or child process result code
    return (pid == -1 ? -1 : pstat);
}

APTH_DEFINE_SYSCALL(
    int, select,
    (int nfd, fd_set *rfds, fd_set *wfds, fd_set *efds, struct timeval *timeout),
    (nfd, rfds, wfds, efds, timeout))
{
    apth_event_t ev;
    apth_t cur = cur_apth();
    apth_debug("apth_syscall_select(hooked): called from thread \"%s\"", cur->name);

    // POSIX.1-2001/SUSv3 compliance
    if (nfd < 0 || nfd > FD_SETSIZE)
        return apth_error(-1, EINVAL);
    if (timeout != NULL)
    {
        // Check timeout sanity
        if (timeout->tv_sec < 0 || timeout->tv_usec < 0 || timeout->tv_usec >= 1000000)
            return apth_error(-1, EINVAL);
        // TODO: why set a month here
        if (timeout->tv_sec > 31 * 24 * 60 * 60) // 1 month
            timeout->tv_sec = 31 * 24 * 60 * 60;
    }

    // First deal with the special situation of a plain microsecond delay
    if (nfd == 0 && rfds == NULL && wfds == NULL && efds == NULL && timeout != NULL)
    {
        if (timeout->tv_sec == 0 && timeout->tv_usec < APTH_SYSCALL_SELECT_DIRECT_TO_SCHED_THRESHOLD_US)
        {
            // Very small delays are acceptable to be performed directly
            while (apth_syscall_raw(select)(0, NULL, NULL, NULL, timeout) < 0 && errno == EINTR)
                ;
        }
        else
        {
            // Larger delays have to go through the scheduler
            ev = apth_event_time(APTH_EVENT_MODE_STATIC, apth_timeout(timeout->tv_sec, timeout->tv_usec));
            apth_wait_event(ev);
        }

        /*
        // POSIX.1-2001/SUSv3 compliance
        if (rfds != NULL)
            FD_ZERO(rfds);
        if (wfds != NULL)
            FD_ZERO(wfds);
        if (efds != NULL)
            FD_ZERO(efds);
        */
        return 0;
    }

    // Now directly poll filedescriptor sets to avoid unnecessary (and resource consuming
    // because of context switches, etc) event handling through the scheduler. We have to
    // be careful here, because not all platforms guarantee us that the sets are unmodified
    // if an error or timeout occurred. So we must prepare another set of them here.
    struct timeval delay;
    fd_set rspare, wspare, espare;
    fd_set *rtmp, *wtmp, *etmp;
    // int selected;
    int rc;

    delay.tv_sec = 0;
    delay.tv_usec = 0;
    rtmp = NULL;
    wtmp = NULL;
    etmp = NULL;
    if (rfds != NULL)
    {
        memcpy(&rspare, rfds, sizeof(fd_set));
        rtmp = &rspare;
    }
    if (wfds != NULL)
    {
        memcpy(&wspare, wfds, sizeof(fd_set));
        wtmp = &wspare;
    }
    if (efds != NULL)
    {
        memcpy(&espare, efds, sizeof(fd_set));
        etmp = &espare;
    }

    while ((rc = apth_syscall_raw(select)(nfd, rtmp, wtmp, etmp, &delay)) < 0 && errno == EINTR)
        ;
    if (rc < 0)
        // Pass-through immediate error
        return apth_error(-1, errno);
    else if (rc > 0 || (rc == 0 && timeout != NULL && apth_time_cmp(timeout, APTH_TIME_ZERO) == 0))
    {
        // Pass-through immediate success
        // Copy back results
        if (rfds != NULL)
            memcpy(rfds, &rspare, sizeof(fd_set));
        if (wfds != NULL)
            memcpy(wfds, &wspare, sizeof(fd_set));
        if (efds != NULL)
            memcpy(efds, &espare, sizeof(fd_set));
        return rc;
    }

    // Suspend currrent apth until one filedescriptor is ready or the timeout occurred.
    apth_event_t ev_select;
    apth_event_t ev_timeout;
    struct list event_list;
    list_init(&event_list);
    rc = -1;
    ev = ev_select = apth_event_select(APTH_EVENT_MODE_STATIC, &rc, nfd, rfds, wfds, efds);
    apth_event_list_add(&event_list, ev);
    ev_timeout = NULL;
    if (timeout != NULL)
    {
        ev_timeout = apth_event_time(APTH_EVENT_MODE_STATIC, apth_timeout(timeout->tv_sec, timeout->tv_usec));
        apth_event_list_add(&event_list, ev_timeout);
    }
    apth_wait_event_list(&event_list);
    if (timeout != NULL)
        apth_event_isolate(ev_timeout);

    // Select return code semantics
    if (ev_select->ev_status == APTH_EV_STATUS_FAILED)
        return apth_error(-1, EBADF);

    // If the select event occurred, then RC should have been set in ev_args.SELECT.n
    // If timeout occurred and select event did not, return 0 and clear fd_set
    if (timeout != NULL &&
        ev_timeout->ev_status == APTH_EV_STATUS_OCCURRED &&
        ev_select->ev_status != APTH_EV_STATUS_OCCURRED)
    {
        if (rfds != NULL)
            FD_ZERO(rfds);
        if (wfds != NULL)
            FD_ZERO(wfds);
        if (efds != NULL)
            FD_ZERO(efds);
        rc = 0;
    }

    return rc;
}

APTH_DEFINE_SYSCALL(
    int, pselect,
    (int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds, const struct timespec *ts, const sigset_t *mask),
    (nfds, rfds, wfds, efds, ts, mask))
{
    TODO("pselect");
}

APTH_FETCH_LIBCFUNC(open)

static int __variadic_open(const char *pathname, int flags, va_list vargs)

// APTH_DEFINE_SYSCALL(int, open, (const char *pathname, int flags, ...), (pathname, flags, ...))
{
    apth_hook_debug(open);

    mode_t mode = 0;
    bool need_mode_arg = (flags & O_CREAT) || (flags & O_TMPFILE);
    if (need_mode_arg)
        mode = va_arg(vargs, mode_t);

    int fd;
    if (need_mode_arg)
        fd = apth_syscall_raw(open)(pathname, flags, mode);
    else
        fd = apth_syscall_raw(open)(pathname, flags);

    if (fd < 0)
        return fd;

    // Register this fd in APTH_FD_TABLE
    if (fd >= 0 && fd < APTH_FD_TABLE_SIZE)
    {
        APTH_FD_TABLE[fd].orig_flags = fcntl(fd, F_GETFL, 0);
        APTH_FD_TABLE[fd].managed = 1;
        APTH_FD_TABLE[fd].refcount = 0;
    }
    return fd;
}

APTH_INTERNAL int apth_syscall(open)(const char *pathname, int flags, ...)
{
    va_list arg_list;
    va_start(arg_list, flags);
    int retval = __variadic_open(pathname, flags, arg_list);
    va_end(arg_list);
    return retval;
}

APTH_INTERNAL int open(const char *pathname, int flags, ...)
{
    va_list arg_list;
    va_start(arg_list, flags);
    int retval = __variadic_open(pathname, flags, arg_list);
    va_end(arg_list);
    return retval;
}

APTH_DEFINE_SYSCALL(int, creat, (const char *pathname, mode_t mode), (pathname, mode))
{
    apth_hook_debug(creat);

    // Invoke libc creat
    int fd = apth_syscall_raw(creat)(pathname, mode);
    if (fd < 0)
        return fd;

    // Register this fd in APTH_FD_TABLE
    if (fd >= 0 && fd < APTH_FD_TABLE_SIZE)
    {
        APTH_FD_TABLE[fd].orig_flags = fcntl(fd, F_GETFL, 0);
        APTH_FD_TABLE[fd].managed = 1;
        APTH_FD_TABLE[fd].refcount = 0;
    }

    return fd;
}

APTH_FETCH_LIBCFUNC(openat)

static int __variadic_openat(int dirfd, const char *pathname, int flags, va_list vargs)
{
    apth_hook_debug(openat);

    mode_t mode = 0;
    bool need_mode_arg = (flags & O_CREAT) || (flags & O_TMPFILE);
    if (need_mode_arg)
        mode = va_arg(vargs, mode_t);

    int fd;
    if (need_mode_arg)
        fd = apth_syscall_raw(openat)(dirfd, pathname, flags, mode);
    else
        fd = apth_syscall_raw(openat)(dirfd, pathname, flags);

    if (fd < 0)
        return fd;

    // Register this fd in APTH_FD_TABLE
    if (fd >= 0 && fd < APTH_FD_TABLE_SIZE)
    {
        APTH_FD_TABLE[fd].orig_flags = fcntl(fd, F_GETFL, 0);
        APTH_FD_TABLE[fd].managed = 1;
        APTH_FD_TABLE[fd].refcount = 0;
    }
    return fd;
}

APTH_INTERNAL int apth_syscall(openat)(int dirfd, const char *pathname, int flags, ...)
{
    va_list arg_list;
    va_start(arg_list, flags);
    int retval = __variadic_openat(dirfd, pathname, flags, arg_list);
    va_end(arg_list);
    return retval;
}

APTH_API int openat(int dirfd, const char *pathname, int flags, ...)
{
    va_list arg_list;
    va_start(arg_list, flags);
    int retval = __variadic_openat(dirfd, pathname, flags, arg_list);
    va_end(arg_list);
    return retval;
}

APTH_DEFINE_SYSCALL(
    int, openat2,
    (int dirfd, const char *pathname, const struct open_how *how, size_t size),
    (dirfd, pathname, how, size))
{
    apth_hook_debug(openat2);

    // Invoke libc socket
    int fd = apth_syscall_raw(openat2)(dirfd, pathname, how, size);
    if (fd < 0)
        return fd;

    // Register this fd in APTH_FD_TABLE
    if (fd >= 0 && fd < APTH_FD_TABLE_SIZE)
    {
        APTH_FD_TABLE[fd].orig_flags = fcntl(fd, F_GETFL, 0);
        APTH_FD_TABLE[fd].managed = 1;
        APTH_FD_TABLE[fd].refcount = 0;
    }

    return fd;
}

APTH_DEFINE_SYSCALL(int, socket,
                    (int domain, int type, int protocol),
                    (domain, type, protocol))
{
    apth_hook_debug(socket);

    // Invoke libc socket
    int fd = apth_syscall_raw(socket)(domain, type, protocol);
    if (fd < 0)
        return fd;

    // Register this fd in APTH_FD_TABLE
    if (fd >= 0 && fd < APTH_FD_TABLE_SIZE)
    {
        APTH_FD_TABLE[fd].orig_flags = fcntl(fd, F_GETFL, 0);
        APTH_FD_TABLE[fd].managed = 1;
        APTH_FD_TABLE[fd].refcount = 0;
    }

    return fd;
}

APTH_DEFINE_SYSCALL(
    int, connect,
    (int fd, const struct sockaddr *address, socklen_t address_len),
    (fd, address, address_len))
{
    apth_hook_debug(connect);
    apth_t cur = cur_apth();
    apth_debug("apth_syscall_connect: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    // Force filedescriptor into non-blocking mode
    int fdmode;
    if ((fdmode = apth_fdmode(fd, APTH_FDMODE_NONBLOCK)) == APTH_FDMODE_ERROR)
        return apth_error(-1, EBADF);

    // Try to connect
    int rv;
    while ((rv = apth_syscall_raw(connect)(fd, (struct sockaddr *)address, address_len)) == -1 && errno == EINTR)
        ;

    // Restore filedescriptor mode
    apth_shield { apth_fdmode(fd, fdmode); }

    // If it is still on progress wait until socket is really writeable
    if (rv == -1 && errno == EINPROGRESS && fdmode != APTH_FDMODE_NONBLOCK)
    {
        apth_event_t ev;
        ev = apth_event_fd(APTH_GOAL_UNTIL_FD_WRITEABLE | APTH_EVENT_MODE_STATIC, fd);
        if (ev == NULL)
            return apth_error(-1, errno);
        apth_wait_event(ev);

        int err;
        socklen_t errlen;
        errlen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&err, &errlen) == -1)
            return -1;
        if (err == 0)
            return 0;
        return apth_error(rv, err);
    }

    apth_debug("apth_syscall_connect: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_SYSCALL(int, accept,
                    (int fd, struct sockaddr *addr, socklen_t *addrlen),
                    (fd, addr, addrlen))
{
    apth_hook_debug(accept);
    apth_t cur = cur_apth();
    apth_debug("apth_syscall_accept: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    // Force filedescriptor into non-blocking mode
    int fdmode;
    if ((fdmode = apth_fdmode(fd, APTH_FDMODE_NONBLOCK)) == APTH_FDMODE_ERROR)
        return apth_error(-1, EBADF);

    // Poll socket via accept
    apth_event_t ev = APTH_EVENT_NULL;
    int rv;
    while ((rv = apth_syscall_raw(accept)(fd, addr, addrlen)) == -1 &&
           (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
           fdmode != APTH_FDMODE_NONBLOCK)
    {
        // Do lazy event allocation
        ev = apth_event_fd(APTH_GOAL_UNTIL_FD_READABLE | APTH_EVENT_MODE_STATIC, fd);
        if (ev == APTH_EVENT_NULL)
            return apth_error(-1, errno);
        // Wait until accept has a chance
        apth_wait_event(ev);
    }

    // Restore filedescriptor mode
    apth_shield
    {
        apth_fdmode(fd, fdmode);
        if (rv != -1)
            apth_fdmode(rv, fdmode);
    }

    apth_debug("apth_syscall_accept: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_SYSCALL(int, close, (int fd), (fd))
{
    apth_hook_debug(close);

    // Clear entry in `APTH_FD_TABLE`
    if (fd >= 0 && fd < APTH_FD_TABLE_SIZE)
    {
        APTH_FD_TABLE[fd].orig_flags = 0;
        APTH_FD_TABLE[fd].managed = 0;
        APTH_FD_TABLE[fd].refcount = 0;
    }

    // TODO(fd): remove fd from all schedulers epoll instances
    // epoll will remove fd when fd close, but would not do this if this fd was
    // ever `dup`ped.
    // For safety, explicitly invoke EPOLL_CTL_DEL and ignore its error

    // NOTE: if there's apths waiting for this `fd`'s I/O event, event
    // manager's select / epoll logic will error to this fd.
    // Then in phase 2 we could handle the situation in `APTH_EV_STATUS_FAILED`
    // branch.

    // Since `close` is a very frequent system call, we must ensure its overhead
    // is very low. Everything else should be handled in event manager.
    apth_sched_t sched = cur_sched();
    // Ignore error.
    int _err = epoll_ctl(sched->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    (void)_err;

    // Invoke libc close
    return apth_syscall_raw(close)(fd);
}

APTH_DEFINE_SYSCALL(ssize_t, read,
                    (int fd, void *buf, size_t nbytes), (fd, buf, nbytes))
{
    apth_hook_debug(read);

    apth_t cur = cur_apth();
    apth_debug("apth_syscall_read: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (nbytes == 0)
        return 0;
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    int orig_mode = apth_fd_acquire(fd);
    if (orig_mode < 0) // APTH_FDMODE_ERROR
        return apth_error(-1, EBADF);

    ssize_t rv;
    for (;;)
    {
        apth_debug("TRY READ FAST PATH");
        // Fast-path : avoid waiting when data is already available.
        while ((rv = apth_syscall_raw(read)(fd, buf, nbytes)) < 0 && errno == EINTR)
            ;
        apth_debug("AFTER TRYED: rv = %d errno = %d", rv, errno);

        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // Data not ready, yield CPU to other apths
            apth_event_t ev = apth_event_fd(APTH_GOAL_UNTIL_FD_READABLE | APTH_EVENT_MODE_STATIC, fd);
            apth_wait_event(ev);
            continue; // Try again after waked
        }

        // rv >= 0 (succeed / EOF) or rv < 0 (real error)
        // Either situation we should return
        break;
    }

    // Restore filedescriptor mode
    apth_fd_release(fd);

    apth_debug("apth_syscall_read: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_SYSCALL(ssize_t, __read_chk,
                    (int fd, void *buf, size_t nbytes, size_t buflen),
                    (fd, buf, nbytes, buflen))
{

    // TODO: perform check
    apth_hook_debug(__read_chk);
    return apth_syscall(read)(fd, buf, nbytes);
}

APTH_DEFINE_SYSCALL(ssize_t, write,
                    (int fd, const void *buf, size_t nbytes), (fd, buf, nbytes))
{
    apth_hook_debug(write);

    apth_t cur = cur_apth();
    apth_debug("apth_syscall_write: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (nbytes == 0)
        return 0;
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    int orig_mode = apth_fd_acquire(fd);
    if (orig_mode < 0) // APTH_FDMODE_ERROR
        return apth_error(-1, EBADF);

    ssize_t rv = 0;
    for (;;)
    {
        // Try directly write first
        ssize_t s;
        while ((s = apth_syscall_raw(write)(fd, buf, nbytes)) < 0 && errno == EINTR)
            ;

        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // FIXME: event allocated more than once
            apth_event_t ev = apth_event_fd(APTH_GOAL_UNTIL_FD_WRITEABLE | APTH_EVENT_MODE_STATIC, fd);
            apth_wait_event(ev);
            continue; // try again
        }

        if (s > 0)
            rv += s;

        // although we're physically now in non-blocking mode,
        // iterate unless all data is written or an error occurs, because
        // we've to mimic the usual blocking I/O behaviour of write(2).
        if (s > 0 && s < (ssize_t)nbytes)
        {
            nbytes -= s;
            buf = (void *)((char *)buf + s);
        }

        // pass error to caller, but not for partial writes (rv > 0)
        if (s < 0 && rv == 0)
            rv = -1;

        break;
    }

    // Restore filedescriptor mode
    apth_fd_release(fd);

    apth_debug("apth_syscall_write: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_SYSCALL(ssize_t, readv,
                    (int fd, const struct iovec *iov, int iovcnt),
                    (fd, iov, iovcnt))
{
    apth_t cur = cur_apth();
    apth_debug("apth_syscall_readv: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (iovcnt <= 0 || iovcnt > UIO_MAXIOV)
        return apth_error(-1, EINVAL);
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    int orig_mode = apth_fd_acquire(fd);
    if (orig_mode < 0) // APTH_FDMODE_ERROR
        return apth_error(-1, EBADF);

    ssize_t rv;
    for (;;)
    {
        // now perform the actual readv operation
        while ((rv = apth_syscall_raw(readv)(fd, iov, iovcnt)) < 0 && errno == EINTR)
            ;

        // EAGAIN / EWOULDBLOCK: fd temporarily not readable; yield and retry.
        // POSIX allows either name; check both for portability.
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            apth_event_t ev = apth_event_fd(APTH_GOAL_UNTIL_FD_READABLE | APTH_EVENT_MODE_STATIC, fd);
            apth_wait_event(ev);
            continue;
        }

        // rv > 0 (data read), rv == 0 (EOF), or rv < 0 (real error)
        // either situation, we should return to caller
        break;
    }

    // Restore filedescriptor mode
    apth_fd_release(fd);

    apth_debug("apth_syscall_readv: leave to thread \"%s\"", cur->name);
    return rv;
}

// Calculate number of bytes in a struct iovec
static ssize_t apth_writev_iov_bytes(const struct iovec *iov, int iovcnt)
{
    ssize_t bytes = 0;
    for (int i = 0; i < iovcnt; i++)
    {
        if (iov[i].iov_len <= 0)
            continue;
        bytes += iov[i].iov_len;
    }
    return bytes;
}

// Advance the virtual pointer of a struct iov
static void apth_writev_iov_advance(const struct iovec *riov, int riovcnt, size_t advance,
                                    struct iovec **liov, int *liovcnt,
                                    struct iovec *tiov, int tiovcnt)
{
    if (*liov == NULL && *liovcnt == 0)
    {
        // Initialize with real (const) structure on first step
        *liov = (struct iovec *)riov;
        *liovcnt = riovcnt;
    }
    if (advance > 0)
    {
        if (*liov == riov && *liovcnt == riovcnt)
        {
            // Reinitialize with a copy to be able to adjust it
            *liov = &tiov[0];
            for (int i = 0; i < riovcnt; i++)
            {
                tiov[i].iov_base = riov[i].iov_base;
                tiov[i].iov_len = riov[i].iov_len;
            }
        }
        // Advance the virtual pointer
        while (*liovcnt > 0 && advance > 0)
        {
            if ((*liov)->iov_len > advance)
            {
                (*liov)->iov_base = (char *)((*liov)->iov_base) + advance;
                (*liov)->iov_len -= advance;
                break;
            }
            else
            {
                advance -= (*liov)->iov_len;
                (*liovcnt)--;
                (*liov)++;
            }
        }
    }
    return;
}

APTH_DEFINE_SYSCALL(ssize_t, writev,
                    (int fd, const struct iovec *iov, int iovcnt),
                    (fd, iov, iovcnt))
{
    apth_t cur = cur_apth();
    apth_debug("apth_syscall_writev: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (iovcnt <= 0 || iovcnt > UIO_MAXIOV)
        return apth_error(-1, EINVAL);
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    int orig_mode = apth_fd_acquire(fd);
    if (orig_mode < 0) // APTH_FDMODE_ERROR
        return apth_error(-1, EBADF);

    // Provide temporary iovec structure for partial-write tracking
    struct iovec tiov_stack[32];
    struct iovec *tiov;
    int tiovcnt;

#define APTH_WRITEV_TIOV_STACK_SIZE (sizeof(tiov_stack) / sizeof(tiov_stack[0]))

    // if (iovcnt > (int)sizeof(tiov_stack))
    if (iovcnt > (int)APTH_WRITEV_TIOV_STACK_SIZE)
    {
        tiovcnt = (sizeof(struct iovec) * UIO_MAXIOV);
        if ((tiov = (struct iovec *)malloc(tiovcnt)) == NULL)
        {
            // apth_shield { apth_fdmode(fd, fdmode); }
            apth_fd_release(fd);
            return apth_error(-1, errno);
        }
    }
    else
    {
        tiovcnt = sizeof(tiov_stack);
        tiov = tiov_stack;
    }

    // Init return value and number of bytes to write
    ssize_t rv = 0;
    size_t nbytes = apth_writev_iov_bytes(iov, iovcnt);

    // Init local iovec structure
    int liovcnt = 0;
    struct iovec *liov = NULL;
    apth_writev_iov_advance(iov, iovcnt, 0, &liov, &liovcnt, tiov, tiovcnt);

    for (;;)
    {
        ssize_t s;
        while ((s = apth_syscall_raw(writev)(fd, liov, liovcnt)) < 0 && errno == EINTR)
            ;

        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            apth_event_t ev = apth_event_fd(
                APTH_GOAL_UNTIL_FD_WRITEABLE | APTH_EVENT_MODE_STATIC, fd);
            apth_wait_event(ev);
            continue;
        }

        if (s > 0)
            rv += s;

        // Although we are physically now in non-blocking mode, iterate unless
        // all data is written or an error occurs, because we have to mimic
        // the usual blocking I/O behaviour of writev(2)
        if (s > 0 && s < (ssize_t)nbytes)
        {
            nbytes -= s;
            apth_writev_iov_advance(iov, iovcnt, s, &liov, &liovcnt, tiov, tiovcnt);
            // n = 0;
            continue;
        }

        // Pass error to caller, but not for partial writes (rv > 0)
        if (s < 0 && rv == 0)
            rv = -1;

        break;
    }

    // Cleanup
    if (iovcnt > (int)APTH_WRITEV_TIOV_STACK_SIZE)
        free(tiov);

    // Restore filedescriptor mode
    apth_fd_release(fd);

    apth_debug("apth_syscall_writev: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_SYSCALL(
    ssize_t, recvfrom,
    (int sockfd, void *buf, size_t nbytes, int flags,
     struct sockaddr *src_addr, socklen_t *addrlen),
    (sockfd, buf, nbytes, flags, src_addr, addrlen))
{
    apth_hook_debug(recvfrom);
    apth_t cur = cur_apth();
    apth_debug("apth_syscall_recvfrom: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (nbytes == 0)
        return 0;
    if (!apth_util_fd_valid(sockfd))
        return apth_error(-1, EBADF);

    int orig_mode = apth_fd_acquire(sockfd);
    if (orig_mode < 0) // APTH_FDMODE_ERROR
        return apth_error(-1, EBADF);

    ssize_t rv;
    for (;;)
    {
        while ((rv = apth_syscall_raw(recvfrom)(sockfd, buf, nbytes, flags, src_addr, addrlen)) < 0 && errno == EINTR)
            ;

        // EAGAIN / EWOULDBLOCK: sockfd temporarily not readable (e.g. another
        // thread consumed the data, or race left fd in NONBLOCK mode).
        // POSIX allows either name; check both for portability.
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            apth_event_t ev = apth_event_fd(APTH_GOAL_UNTIL_FD_READABLE | APTH_EVENT_MODE_STATIC, sockfd);
            apth_wait_event(ev);
            continue;
        }

        // rv > 0 (data received), rv == 0 (EOF/peer closed), or rv < 0 (real error)
        // return to caller, a short recv is valid POSIX
        break;
    }

    // Restore filedescriptor mode
    apth_fd_release(sockfd);

    apth_debug("apth_syscall_recvfrom: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_SYSCALL(
    ssize_t, __recvfrom_chk,
    (int __fd, void *__restrict __buf, size_t __n, size_t __buflen,
     int __flags, struct sockaddr *__addr, socklen_t *__restrict __addr_len),
    (__fd, __buf, __n, __buflen, __flags, __addr, __addr_len))
{
    // TODO: perform check
    return apth_syscall(recvfrom)(__fd, __buf, __n, __flags, __addr, __addr_len);
}

APTH_DEFINE_SYSCALL(
    ssize_t, sendto,
    (int sockfd, const void *buf, size_t nbytes, int flags,
     const struct sockaddr *dest_addr, socklen_t dest_len),
    (sockfd, buf, nbytes, flags, dest_addr, dest_len))
{
    apth_hook_debug(sendto);
    apth_t cur = cur_apth();
    apth_debug("apth_syscall_sendto: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (nbytes == 0)
        return 0;
    if (!apth_util_fd_valid(sockfd))
        return apth_error(-1, EBADF);

    int orig_mode = apth_fd_acquire(sockfd);
    if (orig_mode < 0) // APTH_FDMODE_ERROR
        return apth_error(-1, EBADF);

    ssize_t rv = 0;
    for (;;)
    {
        ssize_t s;
        while ((s = apth_syscall_raw(sendto)(sockfd, buf, nbytes, flags, dest_addr, dest_len)) < 0 && errno == EINTR)
            ;

        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // FIXME: event allocated more than once
            apth_event_t ev = apth_event_fd(APTH_GOAL_UNTIL_FD_WRITEABLE | APTH_EVENT_MODE_STATIC, sockfd);
            apth_wait_event(ev);
            continue; // try again
        }

        if (s > 0)
            rv += s;

        // Although we are physically now in non-blocking mode, iterate unless
        // all data is written or an error occurs, because we have to mimic
        // the usual blocking I/O behaviour of send(2)/write(2).
        if (s > 0 && s < (ssize_t)nbytes)
        {
            nbytes -= s;
            buf = (void *)((char *)buf + s);
            continue;
        }

        // Pass error to caller, but not for partial sends (rv > 0)
        if (s < 0 && rv == 0)
            rv = -1;

        break;
    }

    // Restore filedescriptor mode
    apth_fd_release(sockfd);

    apth_debug("apth_syscall_sendto: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_SYSCALL(ssize_t, recv,
                    (int sockfd, void *buf, size_t len, int flags),
                    (sockfd, buf, len, flags))
{
    apth_hook_debug(recv);
    // Here we use hooked syscall
    // According to manual of recv, recvfrom, recvmsg:
    // The  only  difference  between recv() and read(2) is the presence of flags.
    // With a zero flags argument, recv() is generally equivalent to read(2)
    // (but see NOTES).  Also, the following call
    //        recv(sockfd, buf, len, flags);
    // is equivalent to
    //        recvfrom(sockfd, buf, len, flags, NULL, NULL);
    return apth_syscall(recvfrom)(sockfd, buf, len, flags, NULL, NULL);
}

APTH_DEFINE_SYSCALL(ssize_t, __recv_chk,
                    (int sockfd, void *buf, size_t len, size_t buflen, int flags),
                    (sockfd, buf, len, buflen, flags))
{
    // TODO: Perform check here
    apth_hook_debug(__recv_chk);
    return apth_syscall(recv)(sockfd, buf, len, flags);
}

APTH_DEFINE_SYSCALL(ssize_t, send,
                    (int sockfd, const void *buf, size_t len, int flags),
                    (sockfd, buf, len, flags))
{
    apth_hook_debug(send);
    // Here we use hooked syscall
    // According to manual of send, sendto, sendmsg
    // The  send()  call may be used only when the socket is in a connected state
    // (so that the intended recipient is known).  The only difference between
    // send() and write(2) is the presence of flags.  With a zero flags argument,
    // send() is equivalent to write(2).  Also, the following call
    //        send(sockfd, buf, len, flags);
    // is equivalent to
    //        sendto(sockfd, buf, len, flags, NULL, 0);
    // The argument sockfd is the file descriptor of the sending socket.
    return apth_syscall(sendto)(sockfd, buf, len, flags, NULL, 0);
}

APTH_DEFINE_SYSCALL(int, poll,
                    (struct pollfd * fds, nfds_t nfds, int timeout),
                    (fds, nfds, timeout))
{
    if (nfds == 0)
    {
        if (timeout > 0)
        {
            usleep(timeout * 1000);
        }
        return 0;
    }

    // Do 0-timeout detection first
    int rc;
    while ((rc = apth_syscall_raw(poll)(fds, nfds, 0)) < 0 && errno == EINTR)
        ;
    if (rc > 0 || timeout == 0)
        return rc;

    // Construct event list, one fd event for every pollfd
    struct list event_list;
    list_init(&event_list);
    for (nfds_t i = 0; i < nfds; i++)
    {
        unsigned long goal = APTH_EVENT_MODE_STATIC;
        if (fds[i].events & POLLIN)
            goal |= APTH_GOAL_UNTIL_FD_READABLE;
        if (fds[i].events & POLLOUT)
            goal |= APTH_GOAL_UNTIL_FD_WRITEABLE;
        apth_event_t ev = apth_event_fd(goal, fds[i].fd);
        apth_event_list_add(&event_list, ev);
    }
    if (timeout > 0)
    {
        apth_event_t ev_timeout = apth_event_time(
            APTH_EVENT_MODE_STATIC,
            apth_timeout(timeout / 1000, (timeout % 1000) * 1000));
        apth_event_list_add(&event_list, ev_timeout);
    }
    apth_wait_event_list(&event_list);

    // poll again to fetch revents
    while ((rc = apth_syscall_raw(poll)(fds, nfds, 0)) < 0 && errno == EINTR)
        ;
    return rc;
}

APTH_DEFINE_SYSCALL(
    int, setsockopt,
    (int fd, int level, int option_name, const void *option_value, socklen_t option_len),
    (fd, level, option_name, option_value, option_len))
{
    apth_hook_debug(setsockopt);
    return apth_syscall_raw(setsockopt)(fd, level, option_name, option_value, option_len);
}

// APTH_DEFINE_SYSCALL(int, fcntl, (int fildes, int cmd, ...), (fildes, cmd, __VA_ARGS__))
// {
//     apth_hook_debug(fcntl);
//     TODO("unimplemented fcntl");
// }

APTH_DEFINE_SYSCALL(int, setenv,
                    (const char *n, const char *value, int overwrite),
                    (n, value, overwrite))
{
    apth_hook_debug(setenv);
    TODO("unimplemented setenv");
}

APTH_DEFINE_SYSCALL(int, unsetenv, (const char *n), (n))
{
    apth_hook_debug(unsetenv);
    TODO("unimplemented unsetenv");
}

APTH_DEFINE_SYSCALL(char *, getenv, (const char *n), (n))
{
    apth_hook_debug(getenv);
    TODO("unimplemented getenv");
    return NULL;
}

#undef apth_hook_debug
#undef APTH_DEFINE_SYSCALL
