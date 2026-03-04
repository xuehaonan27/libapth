#include "hook_libc/hook_lowlevel_io.h"
#include "internal_types.h"
#include "internal_funcs.h"

APTH_DEFINE_HOOK(ssize_t, readv,
                 (int fd, const struct iovec *iov, int iovcnt),
                 (fd, iov, iovcnt))
{
    apth_t cur = cur_apth();
    apth_debug("apth_func_readv: enter from thread \"%s\"", cur->name);

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
        while ((rv = apth_func_raw(readv)(fd, iov, iovcnt)) < 0 && errno == EINTR)
            ;

        // EAGAIN / EWOULDBLOCK: fd temporarily not readable; yield and retry.
        // POSIX allows either name; check both for portability.
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            apth_event_t ev = apth_event_fd(APTH_GOAL_UNTIL_FD_READABLE | APTH_EVENT_MODE_STATIC, fd);
            apth_wait_event(ev);
            apth_event_free(ev);
            continue;
        }

        // rv > 0 (data read), rv == 0 (EOF), or rv < 0 (real error)
        // either situation, we should return to caller
        break;
    }

    // Restore filedescriptor mode
    apth_fd_release(fd);

    apth_debug("apth_func_readv: leave to thread \"%s\"", cur->name);
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

APTH_DEFINE_HOOK(ssize_t, writev,
                 (int fd, const struct iovec *iov, int iovcnt),
                 (fd, iov, iovcnt))
{
    apth_t cur = cur_apth();
    apth_debug("apth_func_writev: enter from thread \"%s\"", cur->name);

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
        while ((s = apth_func_raw(writev)(fd, liov, liovcnt)) < 0 && errno == EINTR)
            ;

        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            apth_event_t ev = apth_event_fd(
                APTH_GOAL_UNTIL_FD_WRITEABLE | APTH_EVENT_MODE_STATIC, fd);
            apth_wait_event(ev);
            apth_event_free(ev);
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

    apth_debug("apth_func_writev: leave to thread \"%s\"", cur->name);
    return rv;
}

// TODO: implementation
APTH_DEFINE_HOOK(ssize_t, preadv,
                 (int fd, const struct iovec *iov, int iovcnt, off_t offset),
                 (fd, iov, iovcnt, offset))
{
    TODO("preadv");
}

APTH_DEFINE_HOOK(ssize_t, preadv64,
                 (int fd, const struct iovec *iov, int iovcnt, off64_t offset),
                 (fd, iov, iovcnt, offset))
{
    TODO("preadv64");
}

APTH_DEFINE_HOOK(ssize_t, pwritev,
                 (int fd, const struct iovec *iov, int iovcnt, off_t offset),
                 (fd, iov, iovcnt, offset))
{
    TODO("pwritev");
}

APTH_DEFINE_HOOK(ssize_t, pwritev64,
                 (int fd, const struct iovec *iov, int iovcnt, off64_t offset),
                 (fd, iov, iovcnt, offset))
{
    TODO("pwritev64");
}

APTH_DEFINE_HOOK(ssize_t, preadv2, (int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags), (fd, iov, iovcnt, offset, flags))
{
    TODO("preadv2");
}

APTH_DEFINE_HOOK(ssize_t, preadv64v2,
                 (int fd, const struct iovec *iov, int iovcnt, off64_t offset, int flags),
                 (fd, iov, iovcnt, offset, flags))
{
    TODO("preadv64v2");
}

APTH_DEFINE_HOOK(ssize_t, pwritev2,
                 (int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags),
                 (fd, iov, iovcnt, offset, flags))
{
    TODO("pwritev2");
}

APTH_DEFINE_HOOK(ssize_t, pwritev64v2,
                 (int fd, const struct iovec *iov, int iovcnt, off64_t offset, int flags),
                 (fd, iov, iovcnt, offset, flags))
{
    TODO("pwritevt64v2");
}
