#ifndef __LIBAPTH_INTERNAL_FUNCS_H
#define __LIBAPTH_INTERNAL_FUNCS_H

#include "internal_types.h"

// ============================== Worker ==============================

void worker_key_t_init(void);
apth_worker_t cur_worker(void);
// void set_cur_worker(apth_worker_t worker);
apth_sched_t cur_sched(void);
apth_t cur_apth(void);
void set_cur_apth(apth_t t);
apth_worker_t get_worker_by_id(int worker_id);
int worker_count(void);
static int apth_global_scheduler_pool_init(void);
static int apth_global_scheduler_pool_drop(void);
int add_worker_thread(void);

// ============================== Scheduler ==============================

static bool apth_scheduler_init(apth_sched_t sched, apth_worker_t worker);
static void apth_scheduler_kill(apth_sched_t sched);
static void inc_thrcnt(apth_sched_t sched);
static void dec_thrcnt(apth_sched_t sched);

#define push_apth_to(name) push_apth_to_##name
#define pop_apth_from(name) pop_apth_from_##name
#define head_apth_of(name) head_apth_of_##name
#define DECLARE_SCHED_LIST_OP(name)                         \
    void push_apth_to(name)(apth_t th, apth_sched_t sched); \
    apth_t pop_apth_from(name)(apth_sched_t sched);         \
    apth_t head_apth_of(name)(apth_sched_t sched);
DECLARE_SCHED_LIST_OP(new)
DECLARE_SCHED_LIST_OP(ready)
DECLARE_SCHED_LIST_OP(waiting)
DECLARE_SCHED_LIST_OP(suspended)
DECLARE_SCHED_LIST_OP(terminated)
#undef DECLARE_SCHED_LIST_OP
#undef head_apth_of
#undef pop_apth_from
#undef push_apth_to

static bool apth_sched_is_opening(apth_sched_t sched);
static void apth_sched_calc_load(apth_sched_t sched, apth_time_t *now);
static bool apth_is_not_null_and_valid(apth_t th);
static void *scheduler_routine(void *arg);

// ============================== TCB ==============================
apth_t apth_tcb_alloc(size_t stacksize, void *stackaddr);
void apth_tcb_free(apth_t t);

// ============================== Time ==============================
#define APTH_TIME(sec, usec) {sec, usec}

uint64_t cpu_tick();
apth_time_t apth_time(long sec, long usec);
apth_time_t apth_timeout(long sec, long usec);
void apth_time_set(apth_time_t *t1, apth_time_t *t2);
void apth_time_add(apth_time_t *t1, apth_time_t *t2);
void apth_time_sub(apth_time_t *t1, apth_time_t *t2);
int apth_time_cmp(apth_time_t *t1, apth_time_t *t2);
double apth_time_t2d(apth_time_t *t);

// ============================== Context ==============================
bool apth_ctx_save(apth_cxt_t ctx);
void apth_ctx_restore(apth_cxt_t ctx);
void apth_ctx_switch(apth_cxt_t old, apth_cxt_t new);
bool apth_ctx_set(apth_cxt_t ctx, void (*func)(void), char *stack_addr_lo, char *stack_addr_hi);

// ============================== Cleanup ==============================
void apth_cleanup_popall(apth_t, bool);
static void apth_thread_clenaup(apth_t th);

// ============================== Cancel ==============================
static inline void apth_do_cancel(void *result);

// static inline bool apth_cancel_enabled(int value);
// static inline bool apth_cancel_async_enabled(int value);
// static inline bool apth_cancel_enabled_and_canceled(int value);
// static inline bool apth_cancel_enabled_and_canceled_and_async(int value);

// ============================== Signal ==============================
int apth_util_sigdelete(int sig);

// ============================== TLS ==============================
void apth_key_destroydata(apth_t th);

// ============================== Event ==============================
void apth_sched_eventmanager(apth_sched_t sched, apth_time_t *now, bool dopoll);
void apth_event_list_add(struct list *el, apth_event_t ev);
void apth_event_isolate(apth_event_t ev);
int apth_wait_event_list(struct list *el);
bool apth_wait_event(apth_event_t ev);
apth_event_t apth_event_fd(unsigned long spec, int fd);
apth_event_t apth_event_select(unsigned long spec, int *n, int nfd,
                               fd_set *rfds, fd_set *wfds, fd_set *efds);
apth_event_t apth_event_sigs(unsigned long spec, sigset_t *sigs, int sig);
apth_event_t apth_event_time(unsigned long spec, apth_time_t tv);
apth_event_t apth_event_tid(unsigned long spec, apth_t tid);
apth_event_t apth_event_func(unsigned long spec, apth_event_custom_func_t func, void *arg, apth_time_t tv);

bool apth_event_free(apth_event_t ev);

// ============================== System call ==============================
#define apth_syscall(name) apth_syscall_##name           /* Get reference to APTH wrapped syscall call which is also exposed */
#define apth_syscall_raw(name) apth_syscall_raw_##name   /* Get reference to LIBC system call */
#define apth_syscall_init(name) apth_syscall_init_##name /* Get reference to LIBC system call initializer  */
#define apth_syscall_pfn_t(name) name##_pfn_t            /* Get system call function pointer type */
#define stringify(x) #x                                  /* Stringify the identifier `x` */
#define apth_hook_debug(name) apth_debug("Hook " stringify(name) " succeed")

#define APTH_DECLARE_SYSCALL(rettype, name, ...)              \
    typedef rettype (*apth_syscall_pfn_t(name))(__VA_ARGS__); \
    rettype apth_syscall(name)(__VA_ARGS__);                  \
    static rettype apth_syscall_init(name)(void);             \
    static apth_syscall_pfn_t(name) apth_syscall_raw(name);

#define APTH_DEFINE_SYSCALL(rettype, name, ...)                                                      \
    static apth_syscall_pfn_t(name) apth_syscall_raw(name) = NULL;                                   \
    static rettype apth_syscall_init(name)(void)                                                     \
    {                                                                                                \
        assert_msg(apth_syscall_raw(name) == NULL, "sanity");                                        \
        apth_syscall_pfn_t(name) func = (apth_syscall_pfn_t(name))dlsym(RTLD_NEXT, stringify(name)); \
        apth_debug("found syscall " stringify(name) " = %p", func);                                  \
        apth_syscall_raw(name) = func;                                                               \
        assert_msg(apth_syscall_raw(name) != NULL, "sanity");                                        \
    }                                                                                                \
    rettype apth_syscall(name)(__VA_ARGS__)

/* These headers for syscall declarations. */
#include <dlfcn.h>
#include <sys/socket.h>
#include <poll.h>
#include <netdb.h>
#include <resolv.h>

APTH_DECLARE_SYSCALL(int, nanosleep, const struct timespec *rqtp, struct timespec *rmtp)
APTH_DECLARE_SYSCALL(int, usleep, unsigned int usec)
APTH_DECLARE_SYSCALL(unsigned int, sleep, unsigned int sec)
APTH_DECLARE_SYSCALL(int, pthread_sigmask, int how, const sigset_t *set, sigset_t *oset)
APTH_DECLARE_SYSCALL(int, sigwait, const sigset_t *set, int *sigp)
APTH_DECLARE_SYSCALL(pid_t, waitpid, pid_t wpid, int *status, int options)
APTH_DECLARE_SYSCALL(pid_t, fork, void)
APTH_DECLARE_SYSCALL(int, system, const char *cmd)
#define APTH_SYSCALL_SELECT_DIRECT_TO_SCHED_THRESHOLD_US 10000 // 10ms
APTH_DECLARE_SYSCALL(int, select, int nfd, fd_set *rfds, fd_set *wfds,
                     fd_set *efds, struct timeval *timeout)
APTH_DECLARE_SYSCALL(int, pselect, int nfds, fd_set *rfds, fd_set *wfds,
                     fd_set *efds, const struct timespec *ts, const sigset_t *mask)

APTH_DECLARE_SYSCALL(int, socket, int domain, int type, int protocol)
APTH_DECLARE_SYSCALL(int, connect, int fd, const struct sockaddr *address, socklen_t address_len)
APTH_DECLARE_SYSCALL(int, close, int fd)
APTH_DECLARE_SYSCALL(int, accept, int fd, struct sockaddr *addr, socklen_t *addrlen)
APTH_DECLARE_SYSCALL(ssize_t, read, int fd, void *buf, size_t nbytes)
APTH_DECLARE_SYSCALL(ssize_t, write, int fd, const void *buf, size_t nbytes)
APTH_DECLARE_SYSCALL(ssize_t, readv, int fd, const struct iovec *iov, int iovcnt)
APTH_DECLARE_SYSCALL(ssize_t, writev, int fd, const struct iovec *iov, int iovcnt)

APTH_DECLARE_SYSCALL(ssize_t, sendto, int sockfd, const void *buf, size_t nbytes,
                     int flags, const struct sockaddr *dest_addr, socklen_t dest_len)
APTH_DECLARE_SYSCALL(ssize_t, recvfrom, int sockfd, void *buf, size_t nbytes,
                     int flags, struct sockaddr *src_addr, socklen_t *addrlen)
APTH_DECLARE_SYSCALL(ssize_t, send, int sockfd, const void *buf, size_t len, int flags)
APTH_DECLARE_SYSCALL(ssize_t, recv, int sockfd, void *buf, size_t len, int flags)
APTH_DECLARE_SYSCALL(int, poll, struct pollfd *fds, nfds_t nfds, int timeout)
APTH_DECLARE_SYSCALL(int, setsockopt, int fd, int level, int option_name,
                     const void *option_value, socklen_t option_len)
APTH_DECLARE_SYSCALL(int, fcntl, int fildes, int cmd, ...)
APTH_DECLARE_SYSCALL(int, setenv, const char *n, const char *value, int overwrite)
APTH_DECLARE_SYSCALL(int, unsetenv, const char *n)
APTH_DECLARE_SYSCALL(char *, getenv, const char *n)
APTH_DECLARE_SYSCALL(struct hostent *, gethostbyname, const char *name)

// ============================== Utility ==============================
bool apth_util_fd_valid(int fd);
void apth_util_fds_merge(int nfd,
                         fd_set *ifds1, fd_set *ofds1,
                         fd_set *ifds2, fd_set *ofds2,
                         fd_set *ifds3, fd_set *ofds3);
bool apth_util_fds_test(int nfd,
                        fd_set *ifds1, fd_set *ofds1,
                        fd_set *ifds2, fd_set *ofds2,
                        fd_set *ifds3, fd_set *ofds3);
int apth_util_fds_select(int nfd,
                         fd_set *ifds1, fd_set *ofds1,
                         fd_set *ifds2, fd_set *ofds2,
                         fd_set *ifds3, fd_set *ofds3);
int apth_fdmode(int fd, int newmode);

#endif /* __LIBAPTH_INTERNAL_FUNCS_H */
