#ifndef __LIBAPTH_HOOK_LIBC_HOOK_SOCKET_H
#define __LIBAPTH_HOOK_LIBC_HOOK_SOCKET_H

#include "hook_utils.h"
#include <sys/socket.h>

#define APTH_LIST_OF_HOOK_SOCKET \
    X(socket)                    \
    X(shutdown)                  \
    X(socketpair)                \
    X(connect)                   \
    X(accept)                    \
    X(recv)                      \
    X(__recv_chk)                \
    X(send)                      \
    X(recvfrom)                  \
    X(__recvfrom_chk)            \
    X(sendto)

// ==================== 16.8 Opening and Closing Sockets ====================

APTH_DECLARE_HOOK(int, socket, int namespace, int style, int protocol)

// TODO: implementation
APTH_DECLARE_HOOK(int, shutdown, int fd, int how)
// TODO: implementation
APTH_DECLARE_HOOK(int, socketpair, int domain, int style, int protocol, int filedes[2])

// ==================== 16.9 Using Sockets with Connections ====================
APTH_DECLARE_HOOK(int, connect, int fd, const struct sockaddr *addr, socklen_t length)
APTH_DECLARE_HOOK(int, accept, int fd, struct sockaddr *addr, socklen_t *length)

APTH_DECLARE_HOOK(ssize_t, recv, int sockfd, void *buf, size_t len, int flags)
APTH_DECLARE_HOOK(ssize_t, __recv_chk,
                  int sockfd, void *buf, size_t len, size_t buflen, int flags)
APTH_DECLARE_HOOK(ssize_t, send, int sockfd, const void *buf, size_t len, int flags)

APTH_DECLARE_HOOK(ssize_t, recvfrom, int sockfd, void *buf, size_t nbytes,
                  int flags, struct sockaddr *src_addr, socklen_t *addrlen)
APTH_DECLARE_HOOK(ssize_t, __recvfrom_chk, int __fd, void *__restrict __buf, size_t __n,
                  size_t __buflen, int __flags, struct sockaddr *__addr,
                  socklen_t *__restrict __addr_len);
APTH_DECLARE_HOOK(ssize_t, sendto, int sockfd, const void *buf, size_t nbytes,
                  int flags, const struct sockaddr *dest_addr, socklen_t dest_len)

#endif // __LIBAPTH_HOOK_LIBC_HOOK_SOCKET_H
