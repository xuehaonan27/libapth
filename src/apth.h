#ifndef __LIBAPTH_H
#define __LIBAPTH_H

// Initialize and destruct.
// int apth_init(void) __attribute__((constructor));
// int apth_drop(void) __attribute__((destructor));

int apth_init(void);
int apth_drop(void);

// Thread identifier
typedef struct apth_st *apth_t;
struct apth_st;

// Thread state
typedef enum
{
    APTH_STATE_SCHEDULER = 0, /* the special scheduler thread only       */
    APTH_STATE_NEW,           /* spawned, but still not dispatched       */
    APTH_STATE_READY,         /* ready, waiting to be dispatched         */
    APTH_STATE_WAITING,       /* suspended, waiting until event occurred */
    APTH_STATE_TERMINATED,    /* terminated, waiting to be joined        */
} apth_state_t;

// ==================== Thread Attributes ====================

struct apth_attr_st;
typedef struct apth_attr_st apth_attr_t;

typedef unsigned int apth_key_t;

// ==================== Cleanup ====================
enum
{
    APTH_CANCEL_ENABLE = 0,
#define APTH_CANCEL_ENABLE APTH_CANCEL_ENABLE
    APTH_CANCEL_DISABLE
#define APTH_CANCEL_DISABLE APTH_CANCEL_DISABLE
};
enum
{
    APTH_CANCEL_DEFERRED = 0,
#define APTH_CANCEL_DEFERRED APTH_CANCEL_DEFERRED
    APTH_CANCEL_ASYNCHRONOUS
#define APTH_CANCEL_ASYNCHRONOUS APTH_CANCEL_ASYNCHRONOUS
};

// ==================== System calls ====================

#include <bits/types/sigset_t.h> // for sigset_t
#include <sys/types.h> // for pid_t
#include <sys/select.h> // for fd_set
#include <sys/socket.h> // for socklen_t
#include <sys/poll.h> // for nfds_t
#include <bits/types/struct_iovec.h> // for struct iovec

#define APTH_EXPOSE_DECLARE_SYSCALL(rettype, name, ...) rettype name(__VA_ARGS__);

APTH_EXPOSE_DECLARE_SYSCALL(int, nanosleep, const struct timespec *rqtp, struct timespec *rmtp)
APTH_EXPOSE_DECLARE_SYSCALL(int, usleep, unsigned int usec)
APTH_EXPOSE_DECLARE_SYSCALL(unsigned int, sleep, unsigned int sec)
APTH_EXPOSE_DECLARE_SYSCALL(int, sigwait, const sigset_t *set, int *sigp)
APTH_EXPOSE_DECLARE_SYSCALL(pid_t, waitpid, pid_t wpid, int *status, int options)
APTH_EXPOSE_DECLARE_SYSCALL(pid_t, fork, void)
APTH_EXPOSE_DECLARE_SYSCALL(int, system, const char *cmd)
APTH_EXPOSE_DECLARE_SYSCALL(int, select, int nfd, fd_set *rfds, fd_set *wfds,
                            fd_set *efds, struct timeval *timeout)
APTH_EXPOSE_DECLARE_SYSCALL(int, pselect, int nfds, fd_set *rfds, fd_set *wfds,
                            fd_set *efds, const struct timespec *ts, const sigset_t *mask)
APTH_EXPOSE_DECLARE_SYSCALL(int, socket, int domain, int type, int protocol)
APTH_EXPOSE_DECLARE_SYSCALL(int, connect, int fd, const struct sockaddr *address, socklen_t address_len)
APTH_EXPOSE_DECLARE_SYSCALL(int, close, int fd)
APTH_EXPOSE_DECLARE_SYSCALL(int, accept, int fd, struct sockaddr *addr, socklen_t *addrlen)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, read, int fd, void *buf, size_t nbytes)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, write, int fd, const void *buf, size_t nbytes)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, readv, int fd, const struct iovec *iov, int iovcnt)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, writev, int fd, const struct iovec *iov, int iovcnt)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, sendto, int sockfd, const void *buf, size_t nbytes,
                            int flags, const struct sockaddr *dest_addr, socklen_t dest_len)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, recvfrom, int sockfd, void *buf, size_t nbytes,
                            int flags, struct sockaddr *src_addr, socklen_t *addrlen)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, send, int sockfd, const void *buf, size_t len, int flags)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, recv, int sockfd, void *buf, size_t len, int flags)
APTH_EXPOSE_DECLARE_SYSCALL(int, poll, struct pollfd *fds, nfds_t nfds, int timeout)
APTH_EXPOSE_DECLARE_SYSCALL(int, setsockopt, int fd, int level, int option_name,
                            const void *option_value, socklen_t option_len)
// APTH_EXPOSE_DECLARE_SYSCALL(int, fcntl, int fildes, int cmd, ...)
APTH_EXPOSE_DECLARE_SYSCALL(int, setenv, const char *n, const char *value, int overwrite)
APTH_EXPOSE_DECLARE_SYSCALL(int, unsetenv, const char *n)
APTH_EXPOSE_DECLARE_SYSCALL(char *, getenv, const char *n)
// APTH_EXPOSE_DECLARE_SYSCALL(struct hostent *, gethostbyname, const char *name)

#undef APTH_EXPOSE_DECLARE_SYSCALL

// ==================== Functions ====================

#include <stdbool.h>

int apth_cancel(apth_t th);
bool apth_cleanup_push(void (*func)(void *), void *arg);
bool apth_cleanup_pop(int execute);
int apth_create(apth_t *newthr, const apth_attr_t *attr,
                void *(*start_routine)(void *), void *__arg);
int apth_key_create(apth_key_t *key, void (*destr)(void *));
int apth_key_delete(apth_key_t key);
void *apth_getspecific(apth_key_t key);
int apth_setspecific(apth_key_t key, const void *value);
int apth_detach(apth_t th);
void apth_exit(void *retval);
int apth_join(apth_t tid, void **value);
apth_t apth_self(void);
int apth_setcancelstate(int state, int *oldstate);
int apth_setcanceltype(int type, int *oldtype);
int apth_setname_np(apth_t th, const char *name);
int apth_yield(void);
int apth_kill(apth_t t, int sig);
int apth_equal(apth_t t1, apth_t t2);

// ==================== INCLUDE SYS HEADERS ====================
#include <bits/types/struct_timeval.h>
typedef struct timeval apth_time_t;

#endif /* __LIBAPTH_H */
