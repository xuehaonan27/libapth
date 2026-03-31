#ifndef __LIBAPTH_HOOK_LIBC_HOOK_LOWLEVEL_IO_H
#define __LIBAPTH_HOOK_LIBC_HOOK_LOWLEVEL_IO_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // For O_TMPFILE
#endif
#include <fcntl.h> // For open flags

#include "hook_utils.h"
#ifndef __USE_LARGEFILE64
#define __USE_LARGEFILE64
#include <sys/types.h> // For off64_t
#endif
// #include <linux/openat2.h> // For struct open_how
#include <sys/uio.h>
#include <poll.h>

/**
 * 13 Low-Level Input/Output
 *
 * This chapter describes functions for performing low-level input/output
 * operations on file descriptors. These functions include the primitives for
 * the higher-level I/O functions described in Input/Output on Streams, as
 * well as functions for performing low-level control operations for which
 * there are no equivalents on streams.
 * Stream-level I/O is more flexible and usually more convenient; therefore,
 * programmers generally use the descriptor-level functions only when necessary.
 * These are some of the usual reasons:
 *
 * For reading binary files in large chunks.
 * For reading an entire file into core before parsing it.
 * To perform operations other than data transfer, which can only be done with
 * a descriptor. (You can use fileno to get the descriptor corresponding to a stream.)
 * To pass descriptors to a child process. (The child can create its own stream
 * to use a descriptor that it inherits, but cannot inherit a stream directly.)
 */

#define APTH_LIST_OF_HOOK_LOWLEVEL_IO \
    X(open)                           \
    X(open64)                         \
    X(openat)                         \
    X(openat64)                       \
    X(creat)                          \
    X(creat64)                        \
    X(close)                          \
    X(close_range)                    \
    X(closefrom)                      \
    X(read)                           \
    X(__read_chk)                     \
    X(pread)                          \
    X(__pread_chk)                    \
    X(pread64)                        \
    X(__pread64_chk)                  \
    X(write)                          \
    X(pwrite)                         \
    X(pwrite64)                       \
    X(readv)                          \
    X(writev)                         \
    X(preadv)                         \
    X(preadv64)                       \
    X(pwritev)                        \
    X(pwritev64)                      \
    X(preadv2)                        \
    X(preadv64v2)                     \
    X(pwritev2)                       \
    X(pwritev64v2)                    \
    X(copy_file_range)                \
    X(dup)                            \
    X(dup2)                           \
    X(dup3)                           \
    X(select)                         \
    X(pselect)                        \
    X(poll)                           \
    X(pipe)                           \
    X(fcntl)                          \
    X(epoll_wait)                     \
    X(epoll_pwait)

#ifdef __O_TMPFILE
#define __OPEN_NEEDS_MODE(oflag) \
    (((oflag) & O_CREAT) != 0 || ((oflag) & __O_TMPFILE) == __O_TMPFILE)
#else
#define __OPEN_NEEDS_MODE(oflag) (((oflag) & O_CREAT) != 0)
#endif

// ==================== 13.1 Opening and Closing Files ====================
// Although GLIBC will handle open and its 64 bits variants with complex conditional
// compilations, we just all hook them and simply write duplicated logics and leave
// all other things to GLIBC.
// Besides, since it's hard to handle variadic function, we just manually separate
// APTH_DECLARE_FETCH_LIBCFUNC and apth_func(...) declaration.

APTH_DECLARE_FETCH_LIBCFUNC(int, open, const char *pathname, int flags, ... /* mode_t mode */)
APTH_INTERNAL int apth_func(open)(const char *pathname, int flags, ... /* mode_t mode */);

// TODO: implementation
APTH_DECLARE_FETCH_LIBCFUNC(int, open64, const char *pathname, int flags, ... /* mode_t mode */)
APTH_INTERNAL int apth_func(open64)(const char *pathname, int flags, ... /* mode_t mode */);

APTH_DECLARE_FETCH_LIBCFUNC(int, openat, int dirfd, const char *pathname, int flags, ... /* mode_t mode */)
APTH_INTERNAL int apth_func(openat)(int dirfd, const char *pathname, int flags, ... /* mode_t mode */);

// TODO: implementation
APTH_DECLARE_FETCH_LIBCFUNC(int, openat64, int filedes, const char *filename, int flags, ... /* mode_t mode */)
APTH_INTERNAL int apth_func(openat64)(int dirfd, const char *pathname, int flags, ... /* mode_t mode */);

// APTH_DECLARE_HOOK(int, openat2, int dirfd, const char *pathname, const struct open_how *how, size_t size)

APTH_DECLARE_HOOK(int, creat, const char *pathname, mode_t mode)
// TODO: implementation
APTH_DECLARE_HOOK(int, creat64, const char *filename, mode_t mode)

APTH_DECLARE_HOOK(int, close, int fd)

// TODO: implementation
APTH_DECLARE_HOOK(int, close_range, unsigned int lowfd, unsigned int maxfd, int flags)
// TODO: implementation
APTH_DECLARE_HOOK(void, closefrom, int lowfd)

// ==================== 13.2 Input and Output Primitives ====================
// Notice that GLIBC might enable fotifying, so the binary might not expose `read` symbol.
// Instead, `__read_chk` might be used. So we must hook them as well!

APTH_DECLARE_HOOK(ssize_t, read, int fd, void *buf, size_t nbytes)
APTH_DECLARE_HOOK(ssize_t, __read_chk, int fd, void *buf, size_t nbytes, size_t buflen)
APTH_DECLARE_HOOK(ssize_t, pread, int fd, void *buf, size_t count, off_t offset)
APTH_DECLARE_HOOK(ssize_t, __pread_chk, int fd, void *buf, size_t nbytes, off_t offset, size_t buflen)

// TODO: implementation
APTH_DECLARE_HOOK(ssize_t, pread64, int fd, void *buf, size_t count, off64_t offset)
// TODO: implementation
APTH_DECLARE_HOOK(ssize_t, __pread64_chk, int fd, void *buf, size_t nbytes, off64_t offset, size_t buflen)

APTH_DECLARE_HOOK(ssize_t, write, int fd, const void *buf, size_t nbytes)
APTH_DECLARE_HOOK(ssize_t, pwrite, int fd, const void *buf, size_t count, off_t offset)

// TODO: implementation
APTH_DECLARE_HOOK(ssize_t, pwrite64, int filedes, const void *buffer, size_t size, off64_t offset)

// ==================== 13.6 Fast Scatter-Gather I/O ====================

APTH_DECLARE_HOOK(ssize_t, readv, int fd, const struct iovec *iov, int iovcnt)
APTH_DECLARE_HOOK(ssize_t, writev, int fd, const struct iovec *iov, int iovcnt)

// TODO: implementation
APTH_DECLARE_HOOK(ssize_t, preadv, int fd, const struct iovec *iov, int iovcnt, off_t offset)
// TODO: implementation
APTH_DECLARE_HOOK(ssize_t, preadv64, int fd, const struct iovec *iov, int iovcnt, off64_t offset)
// TODO: implementation
APTH_DECLARE_HOOK(ssize_t, pwritev, int fd, const struct iovec *iov, int iovcnt, off_t offset)
// TODO: implementation
APTH_DECLARE_HOOK(ssize_t, pwritev64, int fd, const struct iovec *iov, int iovcnt, off64_t offset)
// TODO: implementation
APTH_DECLARE_HOOK(ssize_t, preadv2, int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags)
// TODO: implementation
APTH_DECLARE_HOOK(ssize_t, preadv64v2, int fd, const struct iovec *iov, int iovcnt, off64_t offset, int flags)
// TODO: implementation
APTH_DECLARE_HOOK(ssize_t, pwritev2, int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags)
// TODO: implementation
APTH_DECLARE_HOOK(ssize_t, pwritev64v2, int fd, const struct iovec *iov, int iovcnt, off64_t offset, int flags)

// ==================== 13.7 Copying data between two files ====================
// TODO: implementation
APTH_DECLARE_HOOK(ssize_t, copy_file_range, int inputfd, off64_t *inputpos, int outputfd,
                  off64_t *outputpos, size_t length, unsigned int flags /* must be zero */)

// ==================== 13.13 Duplicating Descriptors ====================
APTH_DECLARE_HOOK(int, dup, int old)
APTH_DECLARE_HOOK(int, dup2, int old, int new)
APTH_DECLARE_HOOK(int, dup3, int old, int new, int flags)

// ==================== 13.21 Other low-level-I/O-related functions ====================
#define APTH_SELECT_DIRECT_TO_SCHED_THRESHOLD_US 10000 // 10ms
// TODO: need to be written
APTH_DECLARE_HOOK(int, select, int nfd, fd_set *rfds, fd_set *wfds,
                  fd_set *efds, struct timeval *timeout)

// TOOD: implementation
APTH_DECLARE_HOOK(int, pselect, int nfds, fd_set *rfds, fd_set *wfds,
                  fd_set *efds, const struct timespec *ts, const sigset_t *mask)

// TOOD: need to be written
APTH_DECLARE_HOOK(int, poll, struct pollfd *fds, nfds_t nfds, int timeout)

// ==================== 15.1 Creating a Pipe ====================
// TOOD: implementation
APTH_DECLARE_HOOK(int, pipe, int pipefd[2])

APTH_DECLARE_FETCH_LIBCFUNC(int, fcntl, int fd, int cmd, ... /* arg */)
APTH_INTERNAL int apth_func(fcntl)(int fd, int cmd, ... /* arg */);

// ==================== epoll hooks (JVM NIO support) ====================
#include <sys/epoll.h>

APTH_DECLARE_HOOK(int, epoll_wait, int epfd, struct epoll_event *events,
                  int maxevents, int timeout)
APTH_DECLARE_HOOK(int, epoll_pwait, int epfd, struct epoll_event *events,
                  int maxevents, int timeout, const sigset_t *sigmask)

#endif // __LIBAPTH_HOOK_LIBC_HOOK_LOWLEVEL_IO_H
