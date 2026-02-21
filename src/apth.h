#ifndef __LIBAPTH_H
#define __LIBAPTH_H

// Initialize and destruct.
// int apth_init(void) __attribute__((constructor));
// int apth_drop(void) __attribute__((destructor));

typedef struct
{
    int workers;
    void *(*main_apth)(void *);
    void *main_args;
} apth_init_t;

int apth_initvals_init(apth_init_t *initvals, int workers,
                       void *(*main_apth)(void *), void *main_args);
int apth_init(apth_init_t *initvals);
int apth_drop(void);

// Thread identifier
typedef struct apth_st *apth_t;
struct apth_st;

#define APTH_CANCELED ((void *)-1)

// ==================== Thread Attributes ====================

struct apth_attr_st;
typedef struct apth_attr_st *apth_attr_t;

enum
{
    APTH_CREATE_JOINABLE,
#define APTH_CREATE_JOINABLE APTH_CREATE_JOINABLE
    APTH_CREATE_DETACHED
#define APTH_CREATE_DETACHED APTH_CREATE_DETACHED
};

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

#include <bits/types/sigset_t.h>     // for sigset_t
#include <sys/types.h>               // for pid_t
#include <sys/select.h>              // for fd_set
#include <sys/socket.h>              // for socklen_t
#include <sys/poll.h>                // for nfds_t
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
#include <stdlib.h> /* malloc, free — needed by APTH_MAIN_BEGIN */
#include <sched.h> // For cpu_set_t

int apth_cancel(apth_t th);
void apth_cleanup_push(void (*func)(void *), void *arg);
void apth_cleanup_pop(int execute);
int apth_create(apth_t *newthr, const apth_attr_t *attr,
                void *(*start_routine)(void *), void *arg);
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

int apth_attr_init(apth_attr_t *attr);
int apth_attr_destroy(apth_attr_t *attr);
int apth_attr_getname_np(apth_attr_t *attr, char *buf, size_t len);
int apth_attr_setname_np(apth_attr_t *attr, const char *name);
int apth_attr_getdetachstate(const apth_attr_t *attr, int *detachstate);
int apth_attr_setdetachstate(apth_attr_t *attr, int detachstate);
int apth_attr_getaffinity_np(const apth_attr_t *attr, size_t cpusetsize, cpu_set_t *cpuset);
int apth_attr_setaffinity_np(apth_attr_t *attr, size_t cpusetsize, const cpu_set_t *cpuset);

// ==================== INCLUDE SYS HEADERS ====================
#include <bits/types/struct_timeval.h>
typedef struct timeval apth_time_t;

// ==================== Application Entry Point ====================

/*
 * Internal struct used to pass argc/argv from the real `main` into the
 * user's apth main thread.  Users never touch this directly.
 */
struct __apth_main_args
{
    int argc;
    char **argv;
};

/*
 * apth_config_defaults - fill *cfg with the built-in default values.
 *
 * Called automatically by the default apth_configure() implementation.
 * When future fields are added to apth_init_t, their defaults live here.
 * Users typically do not call this directly, but APTH_CONFIG implementations
 * should call it first so that unmentioned fields always get a sane value.
 */
void apth_config_defaults(apth_init_t *cfg);

/*
 * apth_configure - library configuration hook.
 *
 * The library provides a weak default implementation (1 worker).
 * Override it either by writing your own function, or — more conveniently —
 * by using the APTH_CONFIG(...) macro at file scope.
 *
 * NOTE: main_apth and main_args are set by APTH_MAIN_BEGIN internally;
 *       do NOT set them inside apth_configure.
 */
void apth_configure(apth_init_t *cfg);

/*
 * APTH_CONFIG(stmts) — override library initialisation parameters.
 *
 * Place this macro at file scope, before APTH_MAIN_BEGIN.
 * `stmts` is a sequence of C statements that assign to fields of
 * `apth_init_t *cfg` (the defaults have already been applied via
 * apth_config_defaults before your statements run).
 *
 * Example — use 4 worker threads:
 *
 *   APTH_CONFIG(
 *       cfg->workers = 4;
 *   )
 *
 * As more fields are added to apth_init_t in future releases you can
 * simply add more assignment statements here without touching anything else.
 */
#define APTH_CONFIG(CFG, ...)             \
    void apth_configure(apth_init_t *CFG) \
    {                                     \
        apth_config_defaults(CFG);        \
        __VA_ARGS__                       \
    }

/*
 * APTH_MAIN_BEGIN(argc_name, argv_name) / APTH_MAIN_END
 *
 * Define the application's logical entry point, replacing the need to write
 * a hand-crafted `int main()` + `void *apth_main(void *)` pair.
 *
 * - argc_name / argv_name: names for the command-line argument variables
 *   that will be in scope inside the body (just like in a normal main).
 * - The body may freely use commas (e.g. `int x, y;`) without any
 *   preprocessor quoting tricks.
 * - Use exit() or apth_exit() to return from the body; a plain `return`
 *   is also accepted (the value is treated as a thread exit value).
 *
 * Example:
 *
 *   APTH_CONFIG(cfg->workers = 2;)   // optional
 *
 *   APTH_MAIN_BEGIN(argc, argv)
 *       int x, y;
 *       apth_t thr;
 *       apth_create(&thr, NULL, worker, NULL);
 *       apth_join(thr, NULL);
 *       exit(0);
 *   APTH_MAIN_END
 */
#define APTH_MAIN_BEGIN(argc_name, argv_name)                                   \
    static void *__apth_main_impl__(void *__apth_args__);                       \
    int main(int __apth_argc__, char *__apth_argv__[])                          \
    {                                                                           \
        struct __apth_main_args *__margs__ =                                    \
            (struct __apth_main_args *)malloc(sizeof(struct __apth_main_args)); \
        __margs__->argc = __apth_argc__;                                        \
        __margs__->argv = __apth_argv__;                                        \
        apth_init_t __initvals__;                                               \
        apth_configure(&__initvals__);                                          \
        __initvals__.main_apth = __apth_main_impl__;                            \
        __initvals__.main_args = (void *)__margs__;                             \
        apth_init(&__initvals__);                                               \
        perror("Should not reach here");                                        \
        return 0;                                                               \
    }                                                                           \
    static void *__apth_main_impl__(void *__apth_args__)                        \
    {                                                                           \
        int argc_name =                                                         \
            ((struct __apth_main_args *)__apth_args__)->argc;                   \
        char **argv_name =                                                      \
            ((struct __apth_main_args *)__apth_args__)->argv;                   \
        free(__apth_args__);                                                    \
        (void)argc_name;                                                        \
        (void)argv_name;

/* Closes the function body opened by APTH_MAIN_BEGIN. */
#define APTH_MAIN_END \
    return (void *)0; \
    }

#endif /* __LIBAPTH_H */
