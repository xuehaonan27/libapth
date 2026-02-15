#ifndef __LIBAPTH_INTERNAL_FUNCS_H
#define __LIBAPTH_INTERNAL_FUNCS_H

#include "internal_types.h"
#include "utils/archplattoold.h"

// ============================== Worker ==============================

APTH_INTERNAL void worker_key_t_init(void);
APTH_INTERNAL apth_worker_t cur_worker(void);
APTH_INTERNAL void set_cur_worker(apth_worker_t worker);
APTH_INTERNAL apth_sched_t cur_sched(void);
APTH_INTERNAL void set_cur_sched(apth_sched_t sched);
APTH_INTERNAL apth_t cur_apth(void);
APTH_INTERNAL void set_cur_apth(apth_t t);
APTH_INTERNAL apth_worker_t get_worker_by_id(int worker_id);
APTH_INTERNAL int worker_count(void);
APTH_INTERNAL int apth_global_scheduler_pool_init(void);
APTH_INTERNAL int apth_global_scheduler_pool_drop(void);
APTH_INTERNAL int add_worker_thread(void);

// ============================== Scheduler ==============================

APTH_INTERNAL bool apth_scheduler_init(apth_sched_t sched, apth_worker_t worker);
APTH_INTERNAL void apth_scheduler_kill(apth_sched_t sched);
APTH_INTERNAL void inc_thrcnt(apth_sched_t sched);
APTH_INTERNAL void dec_thrcnt(apth_sched_t sched);

#define push_apth_to(name) push_apth_to_##name
#define pop_apth_from(name) pop_apth_from_##name
#define head_apth_of(name) head_apth_of_##name
#define DECLARE_SCHED_LIST_OP(name)                                       \
    APTH_INTERNAL void push_apth_to(name)(apth_t th, apth_sched_t sched); \
    APTH_INTERNAL apth_t pop_apth_from(name)(apth_sched_t sched);         \
    APTH_INTERNAL apth_t head_apth_of(name)(apth_sched_t sched);
DECLARE_SCHED_LIST_OP(new)
DECLARE_SCHED_LIST_OP(ready)
DECLARE_SCHED_LIST_OP(waiting)
DECLARE_SCHED_LIST_OP(suspended)
DECLARE_SCHED_LIST_OP(terminated)
#undef DECLARE_SCHED_LIST_OP
#undef head_apth_of
#undef pop_apth_from
#undef push_apth_to

APTH_INTERNAL bool apth_sched_is_opening(apth_sched_t sched);
APTH_INTERNAL void apth_sched_calc_load(apth_sched_t sched, apth_time_t *now);
APTH_INTERNAL bool apth_is_not_null_and_valid(apth_t th);
APTH_INTERNAL void *scheduler_routine(void *arg);

// ============================== TCB ==============================
APTH_INTERNAL apth_t apth_tcb_alloc(size_t stacksize, void *stackaddr);
APTH_INTERNAL void apth_tcb_free(apth_t t);

// ============================== Time ==============================
#define APTH_TIME(sec, usec) {sec, usec}

APTH_INTERNAL uint64_t cpu_tick();
APTH_INTERNAL apth_time_t apth_time(long sec, long usec);
APTH_INTERNAL apth_time_t apth_timeout(long sec, long usec);
APTH_INTERNAL void apth_time_set(apth_time_t *t1, apth_time_t *t2);
APTH_INTERNAL void apth_time_add(apth_time_t *t1, apth_time_t *t2);
APTH_INTERNAL void apth_time_sub(apth_time_t *t1, apth_time_t *t2);
APTH_INTERNAL int apth_time_cmp(apth_time_t *t1, apth_time_t *t2);
APTH_INTERNAL double apth_time_t2d(apth_time_t *t);

// ============================== Context ==============================
APTH_INTERNAL bool apth_ctx_save(apth_cxt_t ctx);
APTH_INTERNAL void apth_ctx_restore(apth_cxt_t ctx);
APTH_INTERNAL void apth_ctx_switch(apth_cxt_t old, apth_cxt_t new);
APTH_INTERNAL bool apth_ctx_set(apth_cxt_t ctx, void (*func)(void),
                                char *stack_addr_lo, char *stack_addr_hi);

// ============================== Cleanup ==============================
APTH_INTERNAL void apth_cleanup_popall(apth_t, bool);
APTH_INTERNAL void apth_thread_clenaup(apth_t th);

// ============================== Cancel ==============================
APTH_INTERNAL void apth_do_cancel(void *result);
APTH_INTERNAL void apth_cancel_point(void);

// static inline bool apth_cancel_enabled(int value);
// static inline bool apth_cancel_async_enabled(int value);
// static inline bool apth_cancel_enabled_and_canceled(int value);
// static inline bool apth_cancel_enabled_and_canceled_and_async(int value);

// ============================== Signal ==============================
APTH_INTERNAL int apth_util_sigdelete(int sig);

// ============================== TLS ==============================
APTH_INTERNAL void apth_key_destroydata(apth_t th);

// ============================== Event ==============================
APTH_INTERNAL void apth_sched_eventmanager(apth_sched_t sched, apth_time_t *now, bool dopoll);
APTH_INTERNAL void apth_event_list_add(struct list *el, apth_event_t ev);
APTH_INTERNAL void apth_event_isolate(apth_event_t ev);
APTH_INTERNAL int apth_wait_event_list(struct list *el);
APTH_INTERNAL bool apth_wait_event(apth_event_t ev);
APTH_INTERNAL apth_event_t apth_event_fd(unsigned long spec, int fd);
APTH_INTERNAL apth_event_t apth_event_select(unsigned long spec, int *n, int nfd,
                                             fd_set *rfds, fd_set *wfds, fd_set *efds);
APTH_INTERNAL apth_event_t apth_event_sigs(unsigned long spec, sigset_t *sigs, int *sig);
APTH_INTERNAL apth_event_t apth_event_time(unsigned long spec, apth_time_t tv);
APTH_INTERNAL apth_event_t apth_event_tid(unsigned long spec, apth_t tid);
APTH_INTERNAL apth_event_t apth_event_func(unsigned long spec, apth_event_custom_func_t func,
                                           void *arg, apth_time_t tv);
APTH_INTERNAL bool apth_event_free(apth_event_t ev);

// ============================== System call ==============================
APTH_INTERNAL int apth_syscall_system_init(void);
APTH_INTERNAL int apth_syscall_system_drop(void);

#define apth_syscall(name) apth_syscall_##name           /* Get reference to APTH wrapped syscall call which is also exposed */
#define apth_syscall_raw(name) apth_syscall_raw_##name   /* Get reference to LIBC system call */
#define apth_syscall_init(name) apth_syscall_init_##name /* Get reference to LIBC system call initializer  */
#define apth_syscall_pfn_t(name) name##_pfn_t            /* Get system call function pointer type */
#define stringify(x) #x                                  /* Stringify the identifier `x` */
#define apth_hook_debug(name) apth_debug("Hook " stringify(name) " succeed")

#define APTH_DECLARE_FETCH_LIBCFUNC(rettype, name, ...)       \
    typedef rettype (*apth_syscall_pfn_t(name))(__VA_ARGS__); \
    APTH_INTERNAL int apth_syscall_init(name)(void);          \
    extern apth_syscall_pfn_t(name) apth_syscall_raw(name);

#define APTH_DECLARE_SYSCALL(rettype, name, ...)            \
    APTH_DECLARE_FETCH_LIBCFUNC(rettype, name, __VA_ARGS__) \
    rettype apth_syscall(name)(__VA_ARGS__);

#define APTH_FETCH_LIBCFUNC(rettype, name, ...)                                                      \
    MAYBE_UNUSED APTH_INTERNAL apth_syscall_pfn_t(name) apth_syscall_raw(name) = NULL;               \
    APTH_INTERNAL int apth_syscall_init(name)(void)                                                  \
    {                                                                                                \
        assert_msg(apth_syscall_raw(name) == NULL, "sanity");                                        \
        apth_syscall_pfn_t(name) func = (apth_syscall_pfn_t(name))dlsym(RTLD_NEXT, stringify(name)); \
        if (func == NULL)                                                                            \
            return -1;                                                                               \
        apth_debug("found syscall " stringify(name) " = %p", func);                                  \
        apth_syscall_raw(name) = func;                                                               \
        assert_msg(apth_syscall_raw(name) != NULL, "sanity");                                        \
        return 0;                                                                                    \
    }

#define APTH_DEFINE_SYSCALL(rettype, name, ...)     \
    APTH_FETCH_LIBCFUNC(rettype, name, __VA_ARGS__) \
    rettype apth_syscall(name)(__VA_ARGS__)

/* These headers for syscall declarations. */
#include <dlfcn.h>
#include <sys/socket.h>
#include <poll.h>
#include <netdb.h>
#include <resolv.h>

// ==================== For these functions, only fetch its LIBC impls ====================
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_sigmask, int how, const sigset_t *set, sigset_t *oset)
APTH_DECLARE_FETCH_LIBCFUNC(pthread_t, pthread_self, void)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_kill, pthread_t thread, int sig)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_key_create, pthread_key_t *__key,
                            void (*__destr_function)(void *))
APTH_DECLARE_FETCH_LIBCFUNC(void *, pthread_getspecific, pthread_key_t __key)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_setspecific, pthread_key_t __key,
                            const void *__pointer)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_attr_init, pthread_attr_t *attr)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_join, pthread_t thread, void **retval)

// ==================== For these syscalls, hook them ====================
APTH_DECLARE_SYSCALL(int, nanosleep, const struct timespec *rqtp, struct timespec *rmtp)
APTH_DECLARE_SYSCALL(int, usleep, unsigned int usec)
APTH_DECLARE_SYSCALL(unsigned int, sleep, unsigned int sec)
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
APTH_INTERNAL bool apth_util_fd_valid(int fd);
APTH_INTERNAL void apth_util_fds_merge(int nfd,
                                       fd_set *ifds1, fd_set *ofds1,
                                       fd_set *ifds2, fd_set *ofds2,
                                       fd_set *ifds3, fd_set *ofds3);
APTH_INTERNAL bool apth_util_fds_test(int nfd,
                                      fd_set *ifds1, fd_set *ofds1,
                                      fd_set *ifds2, fd_set *ofds2,
                                      fd_set *ifds3, fd_set *ofds3);
APTH_INTERNAL int apth_util_fds_select(int nfd,
                                       fd_set *ifds1, fd_set *ofds1,
                                       fd_set *ifds2, fd_set *ofds2,
                                       fd_set *ifds3, fd_set *ofds3);
APTH_INTERNAL int apth_fdmode(int fd, int newmode);

#endif /* __LIBAPTH_INTERNAL_FUNCS_H */
