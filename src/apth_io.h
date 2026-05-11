/*
 * apth_io.h — Explicit cooperative I/O API for direct JVM integration.
 *
 * Use these functions instead of raw libc I/O when the calling thread
 * may be an M:N apth.  If the caller is NULL (non-apth) or DEDICATED,
 * the call falls through to raw libc.  Otherwise the FD is set
 * O_NONBLOCK and the thread cooperatively yields on EAGAIN.
 *
 * This API replaces the LD_PRELOAD hook approach: the JVM calls these
 * functions directly, no symbol interposition needed.
 */
#ifndef __LIBAPTH_APTH_IO_H
#define __LIBAPTH_APTH_IO_H

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/epoll.h>
#include <poll.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- read-direction ---------- */
ssize_t apth_io_read(int fd, void *buf, size_t count);
ssize_t apth_io_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t apth_io_readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t apth_io_recv(int sockfd, void *buf, size_t len, int flags);
ssize_t apth_io_recvfrom(int sockfd, void *buf, size_t len, int flags,
                         struct sockaddr *src_addr, socklen_t *addrlen);
ssize_t apth_io_recvmsg(int sockfd, struct msghdr *msg, int flags);

/* ---------- write-direction ---------- */
ssize_t apth_io_write(int fd, const void *buf, size_t count);
ssize_t apth_io_pwrite(int fd, const void *buf, size_t count, off_t offset);
ssize_t apth_io_writev(int fd, const struct iovec *iov, int iovcnt);
ssize_t apth_io_send(int sockfd, const void *buf, size_t len, int flags);
ssize_t apth_io_sendto(int sockfd, const void *buf, size_t len, int flags,
                       const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t apth_io_sendmsg(int sockfd, const struct msghdr *msg, int flags);

/* ---------- connection ---------- */
int apth_io_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int apth_io_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int apth_io_listen(int sockfd, int backlog);

/* ---------- FD lifecycle ---------- */
int apth_io_open(const char *pathname, int flags, ...);
int apth_io_close(int fd);
int apth_io_socket(int domain, int type, int protocol);
int apth_io_socketpair(int domain, int type, int protocol, int sv[2]);
int apth_io_pipe(int pipefd[2]);
int apth_io_dup2(int oldfd, int newfd);

/* ---------- multiplexing ---------- */
int apth_io_poll(struct pollfd *fds, nfds_t nfds, int timeout);
int apth_io_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

#ifdef __cplusplus
}
#endif

#endif /* __LIBAPTH_APTH_IO_H */
