#include "internal_types.h"

#include "common.h"
#include "utils/debug.h"
#include <linux/io_uring.h>

/* These headers for syscall declarations. */
#include <dlfcn.h>
#include <sys/socket.h>
#include <poll.h>
#include <netdb.h>
#include <resolv.h>
#include <pthread.h>

#define apth_syscall(name) apth_syscall_##name           /* Get reference to APTH wrapped syscall call which is also exposed */
#define apth_syscall_raw(name) apth_syscall_raw_##name   /* Get reference to LIBC system call */
#define apth_syscall_init(name) apth_syscall_init_##name /* Get reference to LIBC system call initializer  */
#define apth_syscall_pfn_t(name) name##_pfn_t            /* Get system call function pointer type */
#define stringify(x) #x                                  /* Stringify the identifier `x` */
#define hook_debug(name) apth_debug("Hook " stringify(name) " succeed")

#define DECLARE_SYSCALL(rettype, name, ...)                                                          \
    typedef rettype (*apth_syscall_pfn_t(name))(__VA_ARGS__);                                        \
    static rettype apth_syscall_init(name)(__VA_ARGS__);                                             \
    static apth_syscall_pfn_t(name) apth_syscall_raw(name) = apth_syscall_init(name);                \
    static rettype apth_syscall_init(name)(__VA_ARGS__)                                              \
    {                                                                                                \
        assert_msg(apth_syscall_raw(name) == apth_syscall_init(name), "sanity");                     \
        apth_syscall_pfn_t(name) func = (apth_syscall_pfn_t(name))dlsym(RTLD_NEXT, stringify(name)); \
        apth_debug("found syscall " stringify(name) " = %p", func);                                  \
        apth_syscall_raw(name) = func;                                                               \
        assert_msg(apth_syscall_raw(name) != NULL, "sanity");                                        \
    }                                                                                                \
    rettype apth_syscall(name)(__VA_ARGS__)

DECLARE_SYSCALL(int, socket, int domain, int type, int protocol)
{
    hook_debug(socket);
    TODO("unimplemented socket");
}

DECLARE_SYSCALL(int, connect, int fd, const struct sockaddr *address, socklen_t address_len)
{
    hook_debug(connect);
    TODO("unimplemented connect");
}

DECLARE_SYSCALL(int, close, int fd)
{
    hook_debug(close);
    TODO("unimplemented close");
}

DECLARE_SYSCALL(ssize_t, read, int fildes, void *buf, size_t nbyte)
{
    hook_debug(read);
    TODO("unimplemented read");
}

DECLARE_SYSCALL(ssize_t, write, int fd, const void *buf, size_t nbyte)
{
    hook_debug(write);
    TODO("unimplemented write");
}

DECLARE_SYSCALL(ssize_t, sendto, int socket, const void *message, size_t length,
                int flags, const struct sockaddr *dest_addr, socklen_t dest_len)
{
    hook_debug(sendto);
    TODO("unimplemented sendto");
}

DECLARE_SYSCALL(ssize_t, recvfrom, int socket, void *buffer, size_t length,
                int flags, struct sockaddr *address, socklen_t *address_len)
{
    hook_debug(recvfrom);
    TODO("unimplemented recvfrom");
}

DECLARE_SYSCALL(ssize_t, send, int socket, const void *buffer, size_t length, int flags)
{
    hook_debug(send);
    TODO("unimplemented send");
}

DECLARE_SYSCALL(ssize_t, recv, int socket, void *buffer, size_t length, int flags)
{
    hook_debug(recv);
    TODO("unimplemented recv");
}

DECLARE_SYSCALL(int, poll, struct pollfd fds[], nfds_t nfds, int timeout)
{
    hook_debug(poll);
    TODO("unimplemented poll");
}

DECLARE_SYSCALL(int, setsockopt, int fd, int level, int option_name,
                const void *option_value, socklen_t option_len)
{
    hook_debug(setsockopt);
    TODO("unimplemented setsockopt");
}

DECLARE_SYSCALL(int, fcntl, int fildes, int cmd, ...)
{
    hook_debug(fcntl);
    TODO("unimplemented fcntl");
}

DECLARE_SYSCALL(int, setenv, const char *n, const char *value, int overwrite)
{
    hook_debug(setenv);
    TODO("unimplemented setenv");
}

DECLARE_SYSCALL(int, unsetenv, const char *n)
{
    hook_debug(unsetenv);
    TODO("unimplemented unsetenv");
}

DECLARE_SYSCALL(char *, getenv, const char *n)
{
    hook_debug(getenv);
    TODO("unimplemented getenv");
}

DECLARE_SYSCALL(struct hostent *, gethostbyname, const char *name)
{
    hook_debug(gethostbyname);
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

#undef hook_debug
#undef DECLARE_SYSCALL
