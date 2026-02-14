#include "common.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include <pthread.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <malloc.h>

#define APTH_LIST_OF_SYSCALLS \
    X(nanosleep)              \
    X(usleep)                 \
    X(sleep)                  \
    X(pthread_sigmask)        \
    X(sigwait)                \
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
    X(fcntl)                  \
    X(setenv)                 \
    X(unsetenv)               \
    X(getenv)                 \
    X(gethostbyname)

void apth_syscall_system_init(void)
{
    apth_debug("apth_syscall_system_init: enter");
#define X(name) apth_syscall_init(name)();
    APTH_LIST_OF_SYSCALLS
#undef X
    apth_debug("apth_syscall_system_init: leave");
}

void apth_syscall_system_drop(void)
{
    TODO("apth_syscall_system_drop");
}

// APTH variant of nanosleep(2)
APTH_DEFINE_SYSCALL(int, nanosleep, const struct timespec *rqtp, struct timespec *rmtp)
{
    apth_time_t until;
    apth_time_t offset;
    apth_time_t now;
    apth_event_t ev;

    static apth_key_t ev_key;

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
APTH_DEFINE_SYSCALL(int, usleep, unsigned int usec)
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
    if ((ev = apth_event(APTH_EVENT_MODE_STATIC, until)) == NULL)
        return apth_error(-1, errno);
    apth_wait_event(ev);

    return 0;
}

APTH_DEFINE_SYSCALL(unsigned int, sleep, unsigned int sec)
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
    if ((ev = apth_event(APTH_EVENT_MODE_STATIC, until)) == NULL)
        return sec;
    apth_wait_event(ev);

    return 0;
}

// APTH variant of POSIX pthread_sigmask(3)
APTH_DEFINE_SYSCALL(int, pthread_sigmask, int how, const sigset_t *set, sigset_t *oset)
{
    int rv;

    // Change the explicitly remembered signal mask copy for the scheduler
    TODO("pthread_sigmask");
}

// APTH variant of POSIX sigwait(3)
APTH_DEFINE_SYSCALL(int, sigwait, const sigset_t *set, int *sigp)
{
    apth_event_t ev;
    sigset_t pending;

    if (set == NULL || sigp == NULL)
        return apth_error(EINVAL, EINVAL);

    // Check whether signal is already pending
    if (sigpending(&pending) < 0)
        sigemptyset(&pending);
    for (int sig = 1; sig < APTH_NSIG; sig++)
    {
        if (sigismember(set, sig) && sigismember(&pending, sig))
        {
            apth_util_sigdelete(sig);
            *sigp = sig;
            return 0;
        }
    }

    // Create event and wait on it
    if ((ev = apth_event(APTH_EVENT_MODE_STATIC, set, sigp)) == NULL)
        return apth_error(errno, errno);
    apth_wait_event(ev);

    // nothing to do, scheduler has already set *sigp for us
    return 0;
}

// APTH variant of waitpid(2)
APTH_DEFINE_SYSCALL(pid_t, waitpid, pid_t wpid, int *status, int options)
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
        ev = apth_event(APTH_EVENT_MODE_STATIC, apth_timeout(0, 250000));
        apth_wait_event(ev);
    }

    apth_debug("apth_waitpid: leave to thread \"%s\"", cur->name);
    return pid;
}

APTH_DEFINE_SYSCALL(pid_t, fork, void)
{
    pid_t pid;

    TODO("fork");
}

// APTH variant of system(3)
APTH_DEFINE_SYSCALL(int, system, const char *cmd)
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
        apth_scheduler_pool_kill(); // TODO: implement this

        // Execute the command through Bourne Shell
        execl(APTH_PATH_BINSH, "sh", "-c", cmd, (char *)NULL);

        // POSIX compliant return in case execution failed
        exit(127);
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

APTH_DEFINE_SYSCALL(int, select, int nfd, fd_set *rfds, fd_set *wfds,
                    fd_set *efds, struct timeval *timeout)
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

        // POSIX.1-2001/SUSv3 compliance
        if (rfds != NULL)
            FD_ZERO(rfds);
        if (wfds != NULL)
            FD_ZERO(wfds);
        if (efds != NULL)
            FD_ZERO(efds);
        return 0;
    }

    // Now directly poll filedescriptor sets to avoid unnecessary (and resource consuming
    // because of context switches, etc) event handling through the scheduler. We have to
    // be careful here, because not all platforms guarantee us that the sets are unmodified
    // if an error or timeout occurred. So we must prepare another set of them here.
    struct timeval delay;
    fd_set rspare, wspare, espare;
    fd_set *rtmp, *wtmp, *etmp;
    int selected;
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
    selected = false;
    if (ev_select->ev_status == APTH_EV_STATUS_OCCURRED)
        selected = true;
    if (timeout != NULL && ev_timeout->ev_status == APTH_EV_STATUS_OCCURRED)
    {
        selected = true;
        // POSIX.1-2001/SUSv3 compliance
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

APTH_DEFINE_SYSCALL(int, pselect, int nfds, fd_set *rfds, fd_set *wfds,
                    fd_set *efds, const struct timespec *ts, const sigset_t *mask)
{
    TODO("pselect");
}

APTH_DEFINE_SYSCALL(int, socket, int domain, int type, int protocol)
{
    apth_hook_debug(socket);
    TODO("unimplemented socket");
}

APTH_DEFINE_SYSCALL(int, connect, int fd, const struct sockaddr *address, socklen_t address_len)
{
    apth_hook_debug(connect);
    apth_t cur = cur_apth();
    apth_debug("apth_syscall_connect: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    // Force filedescriptor into non-blocking mode
    int fdmode;
    if ((fdmode == apth_fdmode(fd, APTH_FDMODE_NONBLOCK)) == APTH_FDMODE_ERROR)
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

APTH_DEFINE_SYSCALL(int, accept, int fd, struct sockaddr *addr, socklen_t *addrlen)
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
    while ((rv = apth_syscall_raw(accept)(fd, addr, addrlen)) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK) && fdmode != APTH_FDMODE_NONBLOCK)
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

APTH_DEFINE_SYSCALL(int, close, int fd)
{
    apth_hook_debug(close);
    TODO("unimplemented close");
}

APTH_DEFINE_SYSCALL(ssize_t, read, int fd, void *buf, size_t nbytes)
{
    apth_hook_debug(read);

    apth_t cur = cur_apth();
    apth_debug("apth_syscall_read: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (nbytes == 0)
        return 0;
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    // Check mode of filedescriptor
    int fdmode;
    if ((fdmode = apth_fdmode(fd, APTH_FDMODE_POLL)) == APTH_FDMODE_ERROR)
        return apth_error(-1, EBADF);

    // Poll filedescriptor if not already in non-blocking operation
    if (fdmode == APTH_FDMODE_BLOCK)
    {
        // Now directly poll filedescriptor for readability to avoid
        // unnecessary (and resource consuming because of context switches,
        // etc) event handling through the scheduler
        struct timeval delay;
        apth_event_t ev;
        fd_set fds;
        int n;

        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        delay.tv_sec = 0;
        delay.tv_usec = 0;
        while ((n = apth_syscall_raw(select)(fd + 1, &fds, NULL, NULL, &delay)) < 0 && errno == EINTR)
            ;
        if (n < 0 && (errno == EINVAL || errno == EBADF))
            return apth_error(-1, errno);

        // If filedescriptor is still not readable, let thread sleep until it is
        if (n == 0)
        {
            ev = apth_event_fd(APTH_GOAL_UNTIL_FD_READABLE | APTH_EVENT_MODE_STATIC, fd);
            apth_wait_event(ev);
        }
    }

    // Now perform actual read. We are now guaranteed to not block, either
    // because we were already in non-blocking mode or we determined above
    // by polling that the next read(2) call will not block. But keep in mind,
    // that only 1 next read(2) call is guaranteed to not block (except for
    // the EINTR situation)
    ssize_t rv;
    while ((rv = apth_syscall_raw(read)(fd, buf, nbytes)) < 0 && errno == EINTR)
        ;

    apth_debug("apth_syscall_read: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_SYSCALL(ssize_t, write, int fd, const void *buf, size_t nbytes)
{
    apth_hook_debug(write);

    apth_t cur = cur_apth();
    apth_debug("apth_syscall_write: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (nbytes == 0)
        return 0;
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    // Force filedescriptor into non-blocking mode
    int fdmode;
    ssize_t rv;
    if ((fdmode = apth_fdmode(fd, APTH_FDMODE_NONBLOCK)) == APTH_FDMODE_ERROR)
        return apth_error(-1, EBADF);

    // Poll filedescriptor if not already in non-blocking operation
    if (fdmode != APTH_FDMODE_NONBLOCK)
    {
        // Now directly poll filedescriptor for writeability to avoid
        // unneccessary (and resource consuming because of context switches,
        // etc) event handling through the scheduler
        struct timeval delay;
        apth_event_t ev;
        fd_set fds;
        int n;
        ssize_t s;

        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        delay.tv_sec = 0;
        delay.tv_usec = 0;
        while ((n = apth_syscall_raw(select)(fd + 1, NULL, &fds, NULL, &delay)) < 0 && errno == EINTR)
            ;
        if (n < 0 && (errno == EINVAL || errno == EBADF))
            return pth_error(-1, errno);

        rv = 0;
        for (;;)
        {
            /* if filedescriptor is still not writeable,
               let thread sleep until it is or event occurs */
            if (n < 1)
            {
                ev = apth_event_fd(APTH_GOAL_UNTIL_FD_WRITEABLE | APTH_EVENT_MODE_STATIC, fd);
                apth_wait_event(ev);
            }

            /* now perform the actual write operation */
            while ((s = apth_syscall_raw(write)(fd, buf, nbytes)) < 0 && errno == EINTR)
                ;
            if (s > 0)
                rv += s;

            /* although we're physically now in non-blocking mode,
               iterate unless all data is written or an error occurs, because
               we've to mimic the usual blocking I/O behaviour of write(2). */
            if (s > 0 && s < (ssize_t)nbytes)
            {
                nbytes -= s;
                buf = (void *)((char *)buf + s);
                n = 0;
                continue;
            }

            /* pass error to caller, but not for partial writes (rv > 0) */
            if (s < 0 && rv == 0)
                rv = -1;

            /* stop looping */
            break;
        }
    }
    else
    {
        // In non-blocking mode, just perform the actual write operation
        while ((rv = apth_syscall_raw(write)(fd, buf, nbytes)) < 0 && errno == EINTR)
            ;
    }

    // Restore filedescriptor mode
    apth_shield { apth_fdmode(fd, fdmode); }

    apth_debug("apth_syscall_write: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_SYSCALL(ssize_t, readv, int fd, const struct iovec *iov, int iovcnt)
{
    apth_t cur = cur_apth();
    apth_debug("apth_syscall_readv: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (iovcnt <= 0 || iovcnt > UIO_MAXIOV)
        return apth_error(-1, EINVAL);
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    // Check mode of filedescriptor
    int fdmode;
    if ((fdmode = apth_fdmode(fd, APTH_FDMODE_POLL)) == APTH_FDMODE_ERROR)
        return apth_error(-1, EBADF);

    // Poll filedescriptor if not already in non-blocking operation
    if (fdmode == APTH_FDMODE_BLOCK)
    {
        struct timeval delay;
        apth_event_t ev;
        fd_set fds;
        int n;

        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        delay.tv_sec = 0;
        delay.tv_usec = 0;
        while ((n = apth_syscall_raw(select)(fd + 1, &fds, NULL, NULL, &delay)) < 0 && errno == EINTR)
            ;

        // If filedescriptor is still not readable, let thread sleep until it is
        if (n < 1)
        {
            ev = apth_event_fd(APTH_GOAL_UNTIL_FD_READABLE | APTH_EVENT_MODE_STATIC, fd);
            apth_wait_event(ev);
        }
    }

    ssize_t rv;
    while ((rv = apth_syscall_raw(readv)(fd, iov, iovcnt)) < 0 && errno == EINTR)
        ;

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

APTH_DEFINE_SYSCALL(ssize_t, writev, int fd, const struct iovec *iov, int iovcnt)
{
    apth_t cur = cur_apth();
    apth_debug("apth_syscall_writev: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (iovcnt <= 0 || iovcnt > UIO_MAXIOV)
        return apth_error(-1, EINVAL);
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    // force filedescriptor into non-blocking mode
    int fdmode;
    if ((fdmode = apth_fdmode(fd, APTH_FDMODE_NONBLOCK)) == APTH_FDMODE_ERROR)
        return apth_error(-1, EBADF);

    // Poll filedescriptor if not already in non-blocking operation
    ssize_t rv;
    if (fdmode != APTH_FDMODE_NONBLOCK)
    {
        // Provide temporary iovec structure
        struct iovec tiov_stack[32];
        struct iovec *tiov;
        int tiovcnt;

        if (iovcnt > sizeof(tiov_stack))
        {
            tiovcnt = (sizeof(struct iovec) * UIO_MAXIOV);
            if ((tiov = (struct iovec *)malloc(tiovcnt)) == NULL)
                return apth_error(-1, errno);
        }
        else
        {
            tiovcnt = sizeof(tiov_stack);
            tiov = tiov_stack;
        }

        // Init return value and number of bytes to write
        rv = 0;
        size_t nbytes = apth_writev_iov_bytes(iov, iovcnt);

        // Init local iovec structure
        int liovcnt = 0;
        struct iovec *liov = NULL;
        apth_writev_iov_advance(iov, iovcnt, 0, &liov, &liovcnt, tiov, tiovcnt);

        // First directly poll filedescriptor for writeability to avoid...
        struct timeval delay;
        apth_event_t ev;
        fd_set fds;
        int n;

        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        delay.tv_sec = 0;
        delay.tv_usec = 0;
        while ((n = apth_syscall_raw(select)(fd + 1, NULL, &fds, NULL, &delay)) < 0 && errno == EINTR)
            ;

        for (;;)
        {
            // If filedescriptor is still not writeable, let thread sleep until it is
            if (n < 1)
            {
                ev = apth_event_fd(APTH_GOAL_UNTIL_FD_WRITEABLE | APTH_EVENT_MODE_STATIC, fd);
                apth_wait_event(ev);
            }

            // Now perform actual write operation
            ssize_t s;
            while ((s = apth_syscall_raw(writev)(fd, liov, liovcnt)) < 0 && errno == EINTR)
                ;

            if (s > 0)
                rv += s;

            // Although we are physically now in non-blocking mode, iterate unless
            // all data is written or an error occurs, because we have to mimic
            // the usual blocking I/O behaviour of writev(2)
            if (s > 0 && s < (ssize_t)nbytes)
            {
                nbytes -= s;
                pth_writev_iov_advance(iov, iovcnt, s, &liov, &liovcnt, tiov, tiovcnt);
                n = 0;
                continue;
            }

            // Pass error to caller, but not for partial writes (rv > 0)
            if (s < 0 && rv == 0)
                rv = -1;

            // Stop looping
            break;
        }

        // Cleanup
        if (iovcnt > sizeof(tiov_stack))
            free(tiov);
    }
    else
    {
        // Just perform the actual write operation
        while ((rv = apth_syscall_raw(writev)(fd, iov, iovcnt)) < 0 && errno == EINTR)
            ;
    }

    // Restore filedescriptor mode
    apth_shield { apth_fdmode(fd, fdmode); }

    apth_debug("apth_syscall_writev: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_SYSCALL(ssize_t, recvfrom, int sockfd, void *buf, size_t nbytes,
                    int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
    apth_hook_debug(recvfrom);
    apth_t cur = cur_apth();
    apth_debug("apth_syscall_recvfrom: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (nbytes == 0)
        return 0;
    if (!apth_util_fd_valid(sockfd))
        return apth_error(-1, EBADF);

    // Check mode of filedescriptor
    int fdmode;
    if ((fdmode = apth_fdmode(sockfd, APTH_FDMODE_POLL)) == APTH_FDMODE_ERROR)
        return apth_error(-1, EBADF);

    // Poll filedescriptor if not already in non-blocking operation
    if (fdmode == APTH_FDMODE_BLOCK)
    {
        // Now directly poll filedescriptor for readability to avoid
        // unnecessary (and resource consuming because of context switches,
        // etc) event handling through the scheduler
        if (!apth_util_fd_valid(sockfd))
            return apth_error(-1, EBADF);

        struct timeval delay;
        fd_set fds;
        int n;
        apth_event_t ev;

        FD_ZERO(&fds);
        FD_SET(sockfd, &fds);
        delay.tv_sec = 0;
        delay.tv_usec = 0;

        while ((n = apth_syscall_raw(select)(sockfd + 1, &fds, NULL, NULL, &delay)) < 0 && errno == EINTR)
            ;

        if (n < 0 && (errno == EINVAL || errno == EBADF))
            return apth_error(-1, errno);

        // If filedescriptor is still not readable, let apth sleep until it is
        if (n == 0)
        {
            ev = apth_event_fd(APTH_GOAL_UNTIL_FD_READABLE | APTH_EVENT_MODE_STATIC, sockfd);
            apth_wait_event(ev);
        }
    }

    // Now perform actual read. We are now guaranteed to not block, either because
    // we were already in non-blocking mode or we determined above by polling that
    // the next recvfrom(2) call will not block. But keep in mind, that only 1 next
    // recvfrom(2) call is guaranteed to not block (except for the EINTR situation)
    ssize_t rv;
    while ((rv = apth_syscall_raw(recvfrom)(socket, buf, nbytes, flags, src_addr, addrlen)) < 0 && errno == EINTR)
        ;

    apth_debug("apth_syscall_recvfrom: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_SYSCALL(ssize_t, sendto, int sockfd, const void *buf, size_t nbytes,
                    int flags, const struct sockaddr *dest_addr, socklen_t dest_len)
{
    apth_hook_debug(sendto);
    apth_t cur = cur_apth();
    apth_debug("apth_syscall_sendto: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (nbytes == 0)
        return 0;
    if (!apth_util_fd_valid(sockfd))
        return apth_error(-1, EBADF);

    // Force filedescriptor into non-blocking mode
    int fdmode;
    if ((fdmode = apth_fdmode(sockfd, APTH_FDMODE_NONBLOCK)) == APTH_FDMODE_ERROR)
        return apth_error(-1, EBADF);

    ssize_t rv;
    // Poll filedescriptor if not already in non-blocking operation
    if (fdmode != APTH_FDMODE_NONBLOCK)
    {
        // Now directly poll filedescriptor for writeability to avoid
        // unnecessary (and resource consuming because of context switches,
        // etc) event handling through the scheduler
        if (!apth_util_fd_valid(sockfd))
        {
            apth_fdmode(sockfd, fdmode);
            return apth_error(-1, EBADF);
        }

        struct timeval delay;
        apth_event_t ev;
        fd_set fds;
        ssize_t s;
        int n;

        FD_ZERO(&fds);
        FD_SET(sockfd, &fds);
        delay.tv_sec = 0;
        delay.tv_usec = 0;
        while ((n = apth_syscall_raw(select)(sockfd + 1, NULL, &fds, NULL, &delay)) < 0 && errno == EINTR)
            ;
        if (n < 0 && (errno == EINVAL || errno == EBADF))
            return apth_error(-1, errno);

        rv = 0;
        for (;;)
        {
            // If filedescriptor is still not writable, let apth sleep until it is
            if (n == 0)
            {
                ev = apth_event_fd(APTH_GOAL_UNTIL_FD_WRITEABLE | APTH_EVENT_MODE_STATIC, sockfd);
                apth_wait_event(ev);
            }

            // Now perform the actual send operation
            while ((s = apth_syscall_raw(sendto)(sockfd, buf, nbytes, flags, dest_addr, dest_len)) < 0 && errno == EINTR)
                ;
            if (s > 0)
                rv += s;

            // Although we are physically now in non-blocking mode, iterate unless
            // all data is written or an error occurs, because we have to mimic
            // the usual blocking I/O behaviour of write(2).
            if (s > 0 && s < (ssize_t)nbytes)
            {
                nbytes -= s;
                buf = (void *)((char *)buf + s);
                n = 0;
                continue;
            }

            // Pass error to caller, but not for partial writes (rv > 0)
            if (s < 0 && rv == 0)
                rv = -1;

            // Stop looping
            break;
        }
    }
    else
    {
        // Just perfrom the actual send operation
        while ((rv = apth_syscall_raw(sendto)(sockfd, buf, nbytes, flags, dest_addr, dest_len)) < 0 && errno == EINTR)
            ;
    }

    // Restore filedescriptor mode
    apth_shield { apth_fdmode(sockfd, fdmode); }

    apth_debug("apth_syscall_sendto: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_SYSCALL(ssize_t, recv, int sockfd, void *buf, size_t len, int flags)
{
    apth_hook_debug(recv);
    // Here we use hooked syscall
    return apth_syscall(recvfrom)(sockfd, buf, len, flags, NULL, 0);
}

APTH_DEFINE_SYSCALL(ssize_t, send, int sockfd, const void *buf, size_t len, int flags)
{
    apth_hook_debug(send);
    // Here we use hooked syscall
    return apth_syscall(sendto)(sockfd, buf, len, flags, NULL, 0);
}

APTH_DEFINE_SYSCALL(int, poll, struct pollfd *fds, nfds_t nfds, int timeout)
{
    apth_hook_debug(poll);
    TODO("unimplemented poll");
}

APTH_DEFINE_SYSCALL(int, setsockopt, int fd, int level, int option_name,
                    const void *option_value, socklen_t option_len)
{
    apth_hook_debug(setsockopt);
    TODO("unimplemented setsockopt");
}

APTH_DEFINE_SYSCALL(int, fcntl, int fildes, int cmd, ...)
{
    apth_hook_debug(fcntl);
    TODO("unimplemented fcntl");
}

APTH_DEFINE_SYSCALL(int, setenv, const char *n, const char *value, int overwrite)
{
    apth_hook_debug(setenv);
    TODO("unimplemented setenv");
}

APTH_DEFINE_SYSCALL(int, unsetenv, const char *n)
{
    apth_hook_debug(unsetenv);
    TODO("unimplemented unsetenv");
}

APTH_DEFINE_SYSCALL(char *, getenv, const char *n)
{
    apth_hook_debug(getenv);
    TODO("unimplemented getenv");
}

APTH_DEFINE_SYSCALL(struct hostent *, gethostbyname, const char *name)
{
    apth_hook_debug(gethostbyname);
    TODO("unimplemented gethostbyname");
}

// typedef struct tm *(*localtime_r_pfn_t)(const time_t *timep, struct tm *result);
// typedef void *(*pthread_getspecific_pfn_t)(pthread_key_t key);
// typedef int (*pthread_setspecific_pfn_t)(pthread_key_t key, const void *value);
// typedef res_state (*__res_state_pfn_t)();
// typedef int (*gethostbyname_r_pfn_t)(const char *__restrict name,
//                                      struct hostent *__restrict __result_buf,
//                                      char *__restrict __buf, size_t __buflen,
//                                      struct hostent **__restrict __result,
//                                      int *__restrict __h_errnop);

#undef apth_hook_debug
#undef APTH_DEFINE_SYSCALL
