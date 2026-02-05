#include "internal_types.h"
#include "internal_funcs.h"
#include "common.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include <pthread.h>

void apth_syscall_system_init(void)
{
    TODO("apth_syscall_system_init");
}

// APTH variant of nanosleep(2)
APTH_DEFINE_SYSCALL(int, nanosleep, const struct timespec *rqtp, struct timespec *rmtp)
{
    apth_time_t until;
    apth_time_t offset;
    apth_time_t now;
    apth_event_t ev;

    static apth_key_t ev_key;

    // Consistency checks for POSIX conformance
    if (rqtp == NULL)
        return apth_error(-1, EFAULT);
    if (rqtp->tv_nsec < 0 || rqtp->tv_nsec > (1000 * 1000000))
        return apth_error(-1, EINVAL);

    // Short-circuit
    if (rqtp->tv_sec == 0 && rqtp->tv_nsec == 0)
        return 0;

    // Calculate asleep time
    offset = apth_time((long)(rqtp->tv_sec), (long)(rqtp->tv_nsec) / 1000);
    apth_time_set(&until, APTH_TIME_NOW);
    apth_time_add(&until, &offset);

    // And let apth sleeps until this time is elapsed
    if ((ev = apth_event_time(APTH_EVENT_MODE_STATIC, until)) == NULL)
        return apth_error(-1, errno);
    apth_wait_event(ev);

    // Optionally provide amount of slept time
    if (rmtp != NULL)
    {
        apth_time_set(&now, APTH_TIME_NOW);
        apth_time_sub(&until, &now);
        rmtp->tv_sec = until.tv_sec;
        rmtp->tv_nsec = until.tv_usec * 1000;
    }

    return 0;
}

// APTH variant of usleep(3)
APTH_DEFINE_SYSCALL(int, usleep, unsigned int usec)
{
    apth_time_t until;
    apth_time_t offset;
    apth_event_t ev;

    // Short-circuit
    if (usec == 0)
        return 0;

    // Calculate asleep time
    offset = apth_time((long)(usec / 1000000), (long)(usec % 1000000));
    apth_time_set(&until, APTH_TIME_NOW);
    apth_time_add(&until, &offset);

    /* and let thread sleep until this time is elapsed */
    if ((ev = apth_event(APTH_EVENT_MODE_STATIC, until)) == NULL)
        return apth_error(-1, errno);
    apth_wait_event(ev);

    return 0;
}

APTH_DEFINE_SYSCALL(unsigned int, sleep, unsigned int sec)
{
    apth_time_t until;
    apth_time_t offset;
    apth_event_t ev;

    // Consistency check
    if (sec == 0)
        return 0;

    // Calculate asleep time
    offset = apth_time(sec, 0);
    apth_time_set(&until, APTH_TIME_NOW);
    apth_time_add(&until, &offset);

    // And let thread sleep until this time is elapsed
    if ((ev = apth_event(APTH_EVENT_MODE_STATIC, until)) == NULL)
        return sec;
    apth_wait_event(ev);

    return 0;
}

APTH_DEFINE_SYSCALL(int, socket, int domain, int type, int protocol)
{
    apth_hook_debug(socket);
    TODO("unimplemented socket");
}

APTH_DEFINE_SYSCALL(int, connect, int fd, const struct sockaddr *address, socklen_t address_len)
{
    apth_hook_debug(connect);
    TODO("unimplemented connect");
}

APTH_DEFINE_SYSCALL(int, close, int fd)
{
    apth_hook_debug(close);
    TODO("unimplemented close");
}

APTH_DEFINE_SYSCALL(ssize_t, read, int fildes, void *buf, size_t nbyte)
{
    apth_hook_debug(read);
    TODO("unimplemented read");
}

APTH_DEFINE_SYSCALL(ssize_t, write, int fd, const void *buf, size_t nbyte)
{
    apth_hook_debug(write);
    TODO("unimplemented write");
}

APTH_DEFINE_SYSCALL(ssize_t, sendto, int socket, const void *message, size_t length,
                    int flags, const struct sockaddr *dest_addr, socklen_t dest_len)
{
    apth_hook_debug(sendto);
    TODO("unimplemented sendto");
}

APTH_DEFINE_SYSCALL(ssize_t, recvfrom, int socket, void *buffer, size_t length,
                    int flags, struct sockaddr *address, socklen_t *address_len)
{
    apth_hook_debug(recvfrom);
    TODO("unimplemented recvfrom");
}

APTH_DEFINE_SYSCALL(ssize_t, send, int socket, const void *buffer, size_t length, int flags)
{
    apth_hook_debug(send);
    TODO("unimplemented send");
}

APTH_DEFINE_SYSCALL(ssize_t, recv, int socket, void *buffer, size_t length, int flags)
{
    apth_hook_debug(recv);
    TODO("unimplemented recv");
}

APTH_DEFINE_SYSCALL(int, poll, struct pollfd fds[], nfds_t nfds, int timeout)
{
    apth_hook_debug(poll);
    TODO("unimplemented poll");
}

APTH_DEFINE_SYSCALL(int, setsockopt, int fd, int level, int option_name,
                    const void *option_value, socklen_t option_len)
{
    apth_hook_debug(setsockopt);
    TODO("unimplemented setsockopt");
}

APTH_DEFINE_SYSCALL(int, fcntl, int fildes, int cmd, ...)
{
    apth_hook_debug(fcntl);
    TODO("unimplemented fcntl");
}

APTH_DEFINE_SYSCALL(int, setenv, const char *n, const char *value, int overwrite)
{
    apth_hook_debug(setenv);
    TODO("unimplemented setenv");
}

APTH_DEFINE_SYSCALL(int, unsetenv, const char *n)
{
    apth_hook_debug(unsetenv);
    TODO("unimplemented unsetenv");
}

APTH_DEFINE_SYSCALL(char *, getenv, const char *n)
{
    apth_hook_debug(getenv);
    TODO("unimplemented getenv");
}

APTH_DEFINE_SYSCALL(struct hostent *, gethostbyname, const char *name)
{
    apth_hook_debug(gethostbyname);
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
#undef APTH_DEFINE_SYSCALL
