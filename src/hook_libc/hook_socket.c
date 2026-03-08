#include "hook_socket.h"
#include "internal/types.h"
#include "internal/apth_event.h"
#include "internal/apth_fd.h"

APTH_DEFINE_HOOK(int, socket,
                 (int domain, int style, int protocol),
                 (domain, style, protocol))
{
    apth_hook_debug(socket);

    // Invoke libc socket
    int fd = apth_func_raw(socket)(domain, style, protocol);
    if (fd < 0)
        return fd;

    // Register this fd in APTH_FD_TABLE
    apth_fd_register(fd);

    return fd;
}

APTH_DEFINE_HOOK(int, shutdown, (int sockfd, int how), (sockfd, how))
{
    apth_hook_debug(shutdown);

    // shutdown() doesn't close the fd, it just shuts down the connection
    // No need for fd acquire/release or event handling
    // Just call the raw function directly
    return apth_func_raw(shutdown)(sockfd, how);
}

APTH_DEFINE_HOOK(int, socketpair,
                 (int domain, int style, int protocol, int filedes[2]),
                 (domain, style, protocol, filedes))
{
    apth_hook_debug(socketpair);

    // Call the real socketpair
    int result = apth_func_raw(socketpair)(domain, style, protocol, filedes);
    if (result < 0)
        return result;

    // Register both file descriptors
    apth_fd_register(filedes[0]);
    apth_fd_register(filedes[1]);

    return result;
}

APTH_DEFINE_HOOK(
    int, connect,
    (int fd, const struct sockaddr *addr, socklen_t length),
    (fd, addr, length))
{
    apth_hook_debug(connect);
    apth_t cur = CUR_APTH;
    apth_debug("apth_func_connect: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    apth_fd_register_optional(fd);
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    // Try to connect
    int rv;
    while ((rv = apth_func_raw(connect)(fd, (struct sockaddr *)addr, length)) == -1 && errno == EINTR)
        ;

    // If it is still on progress wait until socket is really writeable
    if (rv == -1 && errno == EINPROGRESS)
    {
        struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_WRITEABLE);
        apth_wait_event(&ev);
        assert(ev.ev_status != APTH_EV_STATUS_PENDING);

        int err;
        socklen_t errlen;
        errlen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&err, &errlen) == -1)
            return -1;

        if (err == 0)
            return 0;
        return apth_error(rv, err);
    }

    apth_debug("apth_func_connect: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_HOOK(int, accept,
                 (int fd, struct sockaddr *addr, socklen_t *length),
                 (fd, addr, length))
{
    apth_hook_debug(accept);
    apth_t cur = CUR_APTH;
    apth_debug("apth_func_accept: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    apth_fd_register_optional(fd);
    if (!apth_util_fd_valid(fd))
        return apth_error(-1, EBADF);

    // Poll socket via accept
    int rv;
    for (;;)
    {
        while ((rv = apth_func_raw(accept)(fd, addr, length)) == -1 && errno == EINTR)
            ;

        // EAGAIN / EWOULDBLOCK: no pending connections
        if (rv == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            struct apth_event_st ev = EVENT_FD(fd, APTH_GOAL_UNTIL_FD_READABLE);
            apth_wait_event(&ev);
            assert(ev.ev_status != APTH_EV_STATUS_PENDING);
            continue;
        }

        // rv >= 0 (new connection) or rv < 0 (real error)
        break;
    }

    // Register the newly accepted connection fd
    if (rv != -1)
        apth_fd_register(rv);

    apth_debug("apth_func_accept: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_HOOK(
    ssize_t, recvfrom,
    (int sockfd, void *buf, size_t nbytes, int flags,
     struct sockaddr *src_addr, socklen_t *addrlen),
    (sockfd, buf, nbytes, flags, src_addr, addrlen))
{
    apth_hook_debug(recvfrom);
    apth_t cur = CUR_APTH;
    apth_debug("apth_func_recvfrom: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (nbytes == 0)
        return 0;
    apth_fd_register_optional(sockfd);
    if (!apth_util_fd_valid(sockfd))
        return apth_error(-1, EBADF);

    ssize_t rv;
    for (;;)
    {
        while ((rv = apth_func_raw(recvfrom)(sockfd, buf, nbytes, flags, src_addr, addrlen)) < 0 && errno == EINTR)
            ;

        // EAGAIN / EWOULDBLOCK: sockfd temporarily not readable (e.g. another
        // thread consumed the data, or race left fd in NONBLOCK mode).
        // POSIX allows either name; check both for portability.
        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            struct apth_event_st ev = EVENT_FD(sockfd, APTH_GOAL_UNTIL_FD_READABLE);
            apth_wait_event(&ev);
            assert(ev.ev_status != APTH_EV_STATUS_PENDING);
            continue;
        }

        // rv > 0 (data received), rv == 0 (EOF/peer closed), or rv < 0 (real error)
        // return to caller, a short recv is valid POSIX
        break;
    }

    apth_debug("apth_func_recvfrom: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_HOOK(
    ssize_t, __recvfrom_chk,
    (int __fd, void *__restrict __buf, size_t __n, size_t __buflen,
     int __flags, struct sockaddr *__addr, socklen_t *__restrict __addr_len),
    (__fd, __buf, __n, __buflen, __flags, __addr, __addr_len))
{
    (void)__buflen;
    return apth_func(recvfrom)(__fd, __buf, __n, __flags, __addr, __addr_len);
}

APTH_DEFINE_HOOK(
    ssize_t, sendto,
    (int sockfd, const void *buf, size_t nbytes, int flags,
     const struct sockaddr *dest_addr, socklen_t dest_len),
    (sockfd, buf, nbytes, flags, dest_addr, dest_len))
{
    apth_hook_debug(sendto);
    apth_t cur = CUR_APTH;
    apth_debug("apth_func_sendto: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (nbytes == 0)
        return 0;
    apth_fd_register_optional(sockfd);
    if (!apth_util_fd_valid(sockfd))
        return apth_error(-1, EBADF);

    ssize_t rv = 0;
    for (;;)
    {
        ssize_t s;
        while ((s = apth_func_raw(sendto)(sockfd, buf, nbytes, flags, dest_addr, dest_len)) < 0 && errno == EINTR)
            ;

        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            struct apth_event_st ev = EVENT_FD(sockfd, APTH_GOAL_UNTIL_FD_WRITEABLE);
            apth_wait_event(&ev);
            assert(ev.ev_status != APTH_EV_STATUS_PENDING);
            continue;
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

    apth_debug("apth_func_sendto: leave to thread \"%s\"", cur->name);
    return rv;
}

APTH_DEFINE_HOOK(ssize_t, recv,
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
    return apth_func(recvfrom)(sockfd, buf, len, flags, NULL, NULL);
}

APTH_DEFINE_HOOK(ssize_t, __recv_chk,
                 (int sockfd, void *buf, size_t len, size_t buflen, int flags),
                 (sockfd, buf, len, buflen, flags))
{
    // TODO: Perform check here
    apth_hook_debug(__recv_chk);
    (void)buflen;
    return apth_func(recv)(sockfd, buf, len, flags);
}

APTH_DEFINE_HOOK(ssize_t, send,
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
    return apth_func(sendto)(sockfd, buf, len, flags, NULL, 0);
}