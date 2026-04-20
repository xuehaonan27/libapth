/*
 * apth_io.c — Explicit cooperative I/O wrappers for direct JVM integration.
 *
 * Every function checks CUR_APTH: NULL or DEDICATED → raw libc,
 * otherwise cooperative (O_NONBLOCK + yield on EAGAIN).
 */
#include "common.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_fd.h"
#include "internal/apth_event.h"
#include "internal/apth_time.h"
#include "hook_libc/hooked_funcs.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include "utils/list.h"
#include "utils/list.inline.h"

#include "apth_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <unistd.h>

/* ===== helpers ===== */

static inline bool __apth_io_is_cooperative(void)
{
    apth_t cur = CUR_APTH;
    return (cur != NULL && !cur->is_dedicated);
}

/* ===== read-direction ===== */

APTH_API ssize_t apth_io_read(int fd, void *buf, size_t count)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(read)(fd, buf, count);

    apth_fd_register_optional(fd);
    ssize_t rv;
    for (;;) {
        while ((rv = apth_func_raw(read)(fd, buf, count)) < 0 && errno == EINTR)
            ;
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_READABLE);
            apth_wait_event(&ev);
            continue;
        }
        break;
    }
    return rv;
}

APTH_API ssize_t apth_io_pread(int fd, void *buf, size_t count, off_t offset)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(pread)(fd, buf, count, offset);

    apth_fd_register_optional(fd);
    ssize_t rv;
    for (;;) {
        while ((rv = apth_func_raw(pread)(fd, buf, count, offset)) < 0 && errno == EINTR)
            ;
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_READABLE);
            apth_wait_event(&ev);
            continue;
        }
        break;
    }
    return rv;
}

APTH_API ssize_t apth_io_readv(int fd, const struct iovec *iov, int iovcnt)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(readv)(fd, iov, iovcnt);

    apth_fd_register_optional(fd);
    ssize_t rv;
    for (;;) {
        while ((rv = apth_func_raw(readv)(fd, iov, iovcnt)) < 0 && errno == EINTR)
            ;
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_READABLE);
            apth_wait_event(&ev);
            continue;
        }
        break;
    }
    return rv;
}

APTH_API ssize_t apth_io_recv(int sockfd, void *buf, size_t len, int flags)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(recv)(sockfd, buf, len, flags);

    apth_fd_register_optional(sockfd);
    ssize_t rv;
    for (;;) {
        while ((rv = apth_func_raw(recv)(sockfd, buf, len, flags)) < 0 && errno == EINTR)
            ;
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct apth_event_st ev = EVENT_FD(sockfd, APTH_GOAL_UNTIL_FD_READABLE);
            apth_wait_event(&ev);
            continue;
        }
        break;
    }
    return rv;
}

APTH_API ssize_t apth_io_recvfrom(int sockfd, void *buf, size_t len, int flags,
                                  struct sockaddr *src_addr, socklen_t *addrlen)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(recvfrom)(sockfd, buf, len, flags, src_addr, addrlen);

    apth_fd_register_optional(sockfd);
    ssize_t rv;
    for (;;) {
        while ((rv = apth_func_raw(recvfrom)(sockfd, buf, len, flags,
                                             src_addr, addrlen)) < 0 && errno == EINTR)
            ;
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct apth_event_st ev = EVENT_FD(sockfd, APTH_GOAL_UNTIL_FD_READABLE);
            apth_wait_event(&ev);
            continue;
        }
        break;
    }
    return rv;
}

APTH_API ssize_t apth_io_recvmsg(int sockfd, struct msghdr *msg, int flags)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(recvmsg)(sockfd, msg, flags);

    apth_fd_register_optional(sockfd);
    ssize_t rv;
    for (;;) {
        while ((rv = apth_func_raw(recvmsg)(sockfd, msg, flags)) < 0 && errno == EINTR)
            ;
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct apth_event_st ev = EVENT_FD(sockfd, APTH_GOAL_UNTIL_FD_READABLE);
            apth_wait_event(&ev);
            continue;
        }
        break;
    }
    return rv;
}

/* ===== write-direction ===== */

APTH_API ssize_t apth_io_write(int fd, const void *buf, size_t count)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(write)(fd, buf, count);

    apth_fd_register_optional(fd);
    ssize_t rv = 0;
    for (;;) {
        ssize_t s;
        while ((s = apth_func_raw(write)(fd, buf, count)) < 0 && errno == EINTR)
            ;
        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_WRITEABLE);
            apth_wait_event(&ev);
            continue;
        }
        if (s > 0) rv += s;
        if (s > 0 && s < (ssize_t)count) {
            count -= s;
            buf = (const char *)buf + s;
            continue;
        }
        if (s < 0 && rv == 0) rv = -1;
        break;
    }
    return rv;
}

APTH_API ssize_t apth_io_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(pwrite)(fd, buf, count, offset);

    apth_fd_register_optional(fd);
    ssize_t rv = 0;
    for (;;) {
        ssize_t s;
        while ((s = apth_func_raw(pwrite)(fd, buf, count, offset)) < 0 && errno == EINTR)
            ;
        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_WRITEABLE);
            apth_wait_event(&ev);
            continue;
        }
        if (s > 0) {
            rv += s;
            if (s < (ssize_t)count) {
                count -= s;
                offset += s;
                buf = (const char *)buf + s;
                continue;
            }
        }
        if (s < 0 && rv == 0) rv = -1;
        break;
    }
    return rv;
}

APTH_API ssize_t apth_io_writev(int fd, const struct iovec *iov, int iovcnt)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(writev)(fd, iov, iovcnt);

    apth_fd_register_optional(fd);
    ssize_t rv;
    for (;;) {
        while ((rv = apth_func_raw(writev)(fd, iov, iovcnt)) < 0 && errno == EINTR)
            ;
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_WRITEABLE);
            apth_wait_event(&ev);
            continue;
        }
        break;
    }
    return rv;
}

APTH_API ssize_t apth_io_send(int sockfd, const void *buf, size_t len, int flags)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(send)(sockfd, buf, len, flags);

    apth_fd_register_optional(sockfd);
    ssize_t rv = 0;
    for (;;) {
        ssize_t s;
        while ((s = apth_func_raw(send)(sockfd, buf, len, flags)) < 0 && errno == EINTR)
            ;
        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct apth_event_st ev = EVENT_FD(sockfd, APTH_GOAL_UNTIL_FD_WRITEABLE);
            apth_wait_event(&ev);
            continue;
        }
        if (s > 0) rv += s;
        if (s > 0 && s < (ssize_t)len) {
            len -= s;
            buf = (const char *)buf + s;
            continue;
        }
        if (s < 0 && rv == 0) rv = -1;
        break;
    }
    return rv;
}

APTH_API ssize_t apth_io_sendto(int sockfd, const void *buf, size_t len, int flags,
                                const struct sockaddr *dest_addr, socklen_t addrlen)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(sendto)(sockfd, buf, len, flags, dest_addr, addrlen);

    apth_fd_register_optional(sockfd);
    ssize_t rv = 0;
    for (;;) {
        ssize_t s;
        while ((s = apth_func_raw(sendto)(sockfd, buf, len, flags,
                                          dest_addr, addrlen)) < 0 && errno == EINTR)
            ;
        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct apth_event_st ev = EVENT_FD(sockfd, APTH_GOAL_UNTIL_FD_WRITEABLE);
            apth_wait_event(&ev);
            continue;
        }
        if (s > 0) rv += s;
        if (s > 0 && s < (ssize_t)len) {
            len -= s;
            buf = (const char *)buf + s;
            continue;
        }
        if (s < 0 && rv == 0) rv = -1;
        break;
    }
    return rv;
}

APTH_API ssize_t apth_io_sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(sendmsg)(sockfd, msg, flags);

    apth_fd_register_optional(sockfd);
    ssize_t rv;
    for (;;) {
        while ((rv = apth_func_raw(sendmsg)(sockfd, msg, flags)) < 0 && errno == EINTR)
            ;
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct apth_event_st ev = EVENT_FD(sockfd, APTH_GOAL_UNTIL_FD_WRITEABLE);
            apth_wait_event(&ev);
            continue;
        }
        break;
    }
    return rv;
}

/* ===== connection ===== */

APTH_API int apth_io_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(connect)(sockfd, addr, addrlen);

    apth_fd_register_optional(sockfd);
    int rv;
    while ((rv = apth_func_raw(connect)(sockfd, (struct sockaddr *)addr, addrlen)) == -1
           && errno == EINTR)
        ;

    if (rv == -1 && errno == EINPROGRESS) {
        struct apth_event_st ev = EVENT_FD(sockfd, APTH_GOAL_UNTIL_FD_WRITEABLE);
        apth_wait_event(&ev);

        int err;
        socklen_t errlen = sizeof(err);
        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1)
            return -1;
        if (err == 0)
            return 0;
        errno = err;
        return -1;
    }
    return rv;
}

APTH_API int apth_io_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(accept)(sockfd, addr, addrlen);

    apth_fd_register_optional(sockfd);
    int rv;
    for (;;) {
        while ((rv = apth_func_raw(accept)(sockfd, addr, addrlen)) < 0 && errno == EINTR)
            ;
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct apth_event_st ev = EVENT_FD(sockfd, APTH_GOAL_UNTIL_FD_READABLE);
            apth_wait_event(&ev);
            continue;
        }
        break;
    }
    if (rv >= 0)
        apth_fd_register(rv);
    return rv;
}

APTH_API int apth_io_listen(int sockfd, int backlog)
{
    return listen(sockfd, backlog);
}

/* ===== FD lifecycle ===== */

APTH_API int apth_io_open(const char *pathname, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | __O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (!__apth_io_is_cooperative())
        return apth_func_raw(open)(pathname, flags, mode);

    int fd = apth_func_raw(open)(pathname, flags | O_NONBLOCK, mode);
    if (fd >= 0)
        apth_fd_register_nonblock(fd);
    return fd;
}

APTH_API int apth_io_close(int fd)
{
    if (__apth_io_is_cooperative()) {
        apth_fd_unregister(fd);
        apth_notify_fd_closed(fd);
    }
    return apth_func_raw(close)(fd);
}

APTH_API int apth_io_socket(int domain, int type, int protocol)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(socket)(domain, type, protocol);

    int fd = apth_func_raw(socket)(domain, type | SOCK_NONBLOCK, protocol);
    if (fd >= 0)
        apth_fd_register_nonblock(fd);
    return fd;
}

APTH_API int apth_io_socketpair(int domain, int type, int protocol, int sv[2])
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(socketpair)(domain, type, protocol, sv);

    int rv = apth_func_raw(socketpair)(domain, type | SOCK_NONBLOCK, protocol, sv);
    if (rv == 0) {
        apth_fd_register_nonblock(sv[0]);
        apth_fd_register_nonblock(sv[1]);
    }
    return rv;
}

APTH_API int apth_io_pipe(int pipefd[2])
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(pipe)(pipefd);

    int rv = pipe2(pipefd, O_NONBLOCK);
    if (rv == 0) {
        apth_fd_register_nonblock(pipefd[0]);
        apth_fd_register_nonblock(pipefd[1]);
    }
    return rv;
}

APTH_API int apth_io_dup2(int oldfd, int newfd)
{
    if (__apth_io_is_cooperative()) {
        if (oldfd != newfd && apth_util_fd_valid(newfd)) {
            apth_fd_unregister(newfd);
            apth_notify_fd_closed(newfd);
        }
    }

    int rv = apth_func_raw(dup2)(oldfd, newfd);
    if (rv >= 0 && __apth_io_is_cooperative())
        apth_fd_register(rv);
    return rv;
}

/* ===== multiplexing ===== */

APTH_API int apth_io_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (!__apth_io_is_cooperative())
        return apth_func_raw(poll)(fds, nfds, timeout);

    if (nfds == 0) {
        if (timeout > 0) {
            struct apth_event_st ev = EVENT_TIME(
                apth_timeout(timeout / 1000, (long)(timeout % 1000) * 1000));
            apth_wait_event(&ev);
        }
        return 0;
    }

    int rc;
    while ((rc = apth_func_raw(poll)(fds, nfds, 0)) < 0 && errno == EINTR)
        ;
    if (rc > 0 || timeout == 0)
        return rc;

    struct apth_event_st evs[nfds];
    struct list event_list;
    list_init(&event_list);

    for (nfds_t i = 0; i < nfds; i++) {
        apth_goal_t goal = 0;
        if (fds[i].events & POLLIN)  goal |= APTH_GOAL_UNTIL_FD_READABLE;
        if (fds[i].events & POLLOUT) goal |= APTH_GOAL_UNTIL_FD_WRITEABLE;
        if (fds[i].events & POLLPRI) goal |= APTH_GOAL_UNTIL_FD_EXCEPTION;
        if (goal == 0) continue;
        apth_fd_register_optional(fds[i].fd);
        evs[i] = (struct apth_event_st)EVENT_FD(fds[i].fd, goal);
        apth_event_list_add(&event_list, &evs[i]);
    }

    struct apth_event_st ev_timeout;
    if (timeout > 0) {
        ev_timeout = (struct apth_event_st)EVENT_TIME(
            apth_timeout(timeout / 1000, (long)(timeout % 1000) * 1000));
        apth_event_list_add(&event_list, &ev_timeout);
    }

    apth_wait_event_list(&event_list);

    while ((rc = apth_func_raw(poll)(fds, nfds, 0)) < 0 && errno == EINTR)
        ;
    return rc;
}
