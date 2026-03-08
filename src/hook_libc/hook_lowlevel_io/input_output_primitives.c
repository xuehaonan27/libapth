#include "hook_libc/hook_lowlevel_io.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_event.h"
#include "internal/apth_fd.h"

APTH_DEFINE_HOOK(ssize_t, read,
                 (int fd, void *buf, size_t nbytes), (fd, buf, nbytes))
{
    apth_hook_debug(read);

    apth_t cur = CUR_APTH;
    apth_debug("apth_func_read: enter from thread \"%s\"", cur->name);

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
        // Fast-path : avoid waiting when data is already available.
        while ((rv = apth_func_raw(read)(fd, buf, nbytes)) < 0 && errno == EINTR)
            ;
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // Data not ready, yield CPU to other apths
            struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_READABLE);
            apth_wait_event(&ev);
            assert(ev.ev_status != APTH_EV_STATUS_PENDING);
            continue; // Try again after waked
        }

        // rv >= 0 (succeed / EOF) or rv < 0 (real error)
        // Either situation we should return
        break;
    }

    // Restore filedescriptor mode
    apth_fd_release(fd);

    apth_debug("apth_func_read: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_HOOK(ssize_t, __read_chk,
                 (int fd, void *buf, size_t nbytes, size_t buflen),
                 (fd, buf, nbytes, buflen))
{

    // TODO: perform check
    apth_hook_debug(__read_chk);
    (void)buflen;
    return apth_func(read)(fd, buf, nbytes);
}

APTH_DEFINE_HOOK(ssize_t, write,
                 (int fd, const void *buf, size_t nbytes), (fd, buf, nbytes))
{
    apth_hook_debug(write);

    apth_t cur = CUR_APTH;
    apth_debug("apth_func_write: enter from thread \"%s\"", cur->name);

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
        while ((s = apth_func_raw(write)(fd, buf, nbytes)) < 0 && errno == EINTR)
            ;

        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_WRITEABLE);
            apth_wait_event(&ev);
            assert(ev.ev_status != APTH_EV_STATUS_PENDING);
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
            continue;
        }

        // pass error to caller, but not for partial writes (rv > 0)
        if (s < 0 && rv == 0)
            rv = -1;

        break;
    }

    // Restore filedescriptor mode
    apth_fd_release(fd);

    apth_debug("apth_func_write: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_HOOK(ssize_t, pread,
                 (int fd, void *buf, size_t count, off_t offset),
                 (fd, buf, count, offset))
{
    apth_hook_debug(pread);

    apth_t cur = CUR_APTH;
    apth_debug("apth_func_pread: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (count == 0)
        return 0;
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    int orig_mode = apth_fd_acquire(fd);
    if (orig_mode < 0) // APTH_FDMODE_ERROR
        return apth_error(-1, EBADF);

    ssize_t rv;
    for (;;)
    {
        while ((rv = apth_func_raw(pread)(fd, buf, count, offset)) < 0 && errno == EINTR)
            ;

        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // Data not ready, yield CPU to other apths
            struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_READABLE);
            apth_wait_event(&ev);
            assert(ev.ev_status != APTH_EV_STATUS_PENDING);
            continue; // Try again after waked
        }

        // rv >= 0 (succeed / EOF) or rv < 0 (real error)
        // Either situation we should return
        break;
    }
    // Restore filedescriptor mode
    apth_fd_release(fd);

    apth_debug("apth_func_pread: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_HOOK(ssize_t, __pread_chk,
                 (int fd, void *buf, size_t nbytes, off_t offset, size_t buflen),
                 (fd, buf, nbytes, offset, buflen))
{
    // TODO: perform check
    apth_hook_debug(__pread_chk);
    (void)buflen;
    return apth_func(pread)(fd, buf, nbytes, offset);
}

APTH_DEFINE_HOOK(ssize_t, pread64,
                 (int fd, void *buf, size_t count, off64_t offset),
                 (fd, buf, count, offset))
{
    apth_hook_debug(pread64);

    apth_t cur = CUR_APTH;
    apth_debug("apth_func_pread64: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (count == 0)
        return 0;
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    int orig_mode = apth_fd_acquire(fd);
    if (orig_mode < 0) // APTH_FDMODE_ERROR
        return apth_error(-1, EBADF);

    ssize_t rv;
    for (;;)
    {
        while ((rv = apth_func_raw(pread64)(fd, buf, count, offset)) < 0 && errno == EINTR)
            ;

        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // Data not ready, yield CPU to other apths
            struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_READABLE);
            apth_wait_event(&ev);
            assert(ev.ev_status != APTH_EV_STATUS_PENDING);
            continue; // Try again after waked
        }

        // rv >= 0 (succeed / EOF) or rv < 0 (real error)
        // Either situation we should return
        break;
    }
    // Restore filedescriptor mode
    apth_fd_release(fd);

    apth_debug("apth_func_pread64: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_HOOK(ssize_t, __pread64_chk,
                 (int fd, void *buf, size_t nbytes, off64_t offset, size_t buflen),
                 (fd, buf, nbytes, offset, buflen))
{
    apth_hook_debug(__pread64_chk);
    (void)buflen;
    return apth_func(pread64)(fd, buf, nbytes, offset);
}

APTH_DEFINE_HOOK(ssize_t, pwrite,
                 (int fd, const void *buf, size_t count, off_t offset),
                 (fd, buf, count, offset))
{
    apth_hook_debug(pwrite);
    apth_t cur = CUR_APTH;
    apth_debug("apth_func_pwrite: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (count == 0)
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
        while ((s = apth_func_raw(pwrite)(fd, buf, count, offset)) < 0 && errno == EINTR)
            ;

        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_WRITEABLE);
            apth_wait_event(&ev);
            assert(ev.ev_status != APTH_EV_STATUS_PENDING);
            continue; // try again
        }

        if (s > 0)
            rv += s;

        // although we're physically now in non-blocking mode,
        // iterate unless all data is written or an error occurs, because
        // we've to mimic the usual blocking I/O behaviour of write(2).
        if (s > 0 && s < (ssize_t)count)
        {
            count -= s;
            buf = (void *)((char *)buf + s);
            offset += s;
            continue;
        }

        // pass error to caller, but not for partial writes (rv > 0)
        if (s < 0 && rv == 0)
            rv = -1;

        break;
    }

    // Restore filedescriptor mode
    apth_fd_release(fd);

    apth_debug("apth_func_pwrite: leave to thread \"%s\"", cur->name);
    return rv;
}

// TODO: implementation
APTH_DEFINE_HOOK(ssize_t, pwrite64,
                 (int filedes, const void *buffer, size_t size, off64_t offset),
                 (filedes, buffer, size, offset))
{
    apth_hook_debug(pwrite64);
    apth_t cur = CUR_APTH;
    apth_debug("apth_func_pwrite64: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (size == 0)
        return 0;
    if (!apth_util_fd_valid(filedes))
        return apth_error(-1, EBADF);

    int orig_mode = apth_fd_acquire(filedes);
    if (orig_mode < 0) // APTH_FDMODE_ERROR
        return apth_error(-1, EBADF);

    ssize_t rv = 0;
    for (;;)
    {
        // Try directly write first
        ssize_t s;
        while ((s = apth_func_raw(pwrite64)(filedes, buffer, size, offset)) < 0 && errno == EINTR)
            ;

        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            struct apth_event_st ev = EVENT_FD(filedes, APTH_GOAL_UNTIL_FD_WRITEABLE);
            apth_wait_event(&ev);
            assert(ev.ev_status != APTH_EV_STATUS_PENDING);
            continue; // try again
        }

        if (s > 0)
            rv += s;

        // although we're physically now in non-blocking mode,
        // iterate unless all data is written or an error occurs, because
        // we've to mimic the usual blocking I/O behaviour of write(2).
        if (s > 0 && s < (ssize_t)size)
        {
            size -= s;
            buffer = (void *)((char *)buffer + s);
            offset += s;
            continue;
        }

        // pass error to caller, but not for partial writes (rv > 0)
        if (s < 0 && rv == 0)
            rv = -1;

        break;
    }

    // Restore filedescriptor mode
    apth_fd_release(filedes);

    apth_debug("apth_func_pwrite64: leave to thread \"%s\"", cur->name);
    return rv;
}