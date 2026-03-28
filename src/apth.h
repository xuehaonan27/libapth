#ifndef __LIBAPTH_H
#define __LIBAPTH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <signal.h>  /* sigset_t, needed by APIs below */
#include <sched.h>   /* cpu_set_t */

// Size definitions for x86-64 Linux GNU (leave room for other platforms)
#if defined(__x86_64__) && defined(__linux__)
#define __SIZEOF_APTH_ATTR_T 256
#define __SIZEOF_APTH_MUTEX_T 64
#define __SIZEOF_APTH_MUTEXATTR_T 4
#define __SIZEOF_APTH_COND_T 48
#define __SIZEOF_APTH_CONDATTR_T 4
#define __SIZEOF_APTH_BARRIER_T 56
#define __SIZEOF_APTH_SEM_T 48
#define __SIZEOF_APTH_RWLOCK_T 88
#else
#error "Unsupported platform - please define sizes for your architecture"
#endif

// Thread attributes (forward declaration needed for apth_init_t)
typedef union
{
    char __size[__SIZEOF_APTH_ATTR_T];
    long int __align;
} apth_attr_t;

typedef struct
{
    int workers;
    void *(*main_apth)(void *);
    void *main_args;
    apth_attr_t *main_attr;
} apth_init_t;

int apth_initvals_init(apth_init_t *initvals, int workers,
                       void *(*main_apth)(void *), void *main_args,
                       apth_attr_t *main_attr);
void apth_init(apth_init_t *initvals);
void apth_drop(void);

// Library-mode initialization: start LIBAPTH schedulers in the background.
// The caller's thread continues as a regular pthread and can create apth
// threads via apth_create().  Call apth_drop() to shut down.
// Returns 0 on success, -1 on error.
int apth_init_library(int workers);

// Thread identifier
typedef struct apth_st *apth_t;
struct apth_st;

#define APTH_CANCELED ((void *)-1)

// Special return value from apth_attr_getsigmask_np if the signal
// mask has not been set.
#define APTH_ATTR_NO_SIGMASK_NP (-1)

// ==================== Thread Class ====================
// Thread scheduling class: hints to the scheduler about workload type.
// IO_BOUND threads get priority dispatch (front of ready queue) when
// waking from I/O or RDMA, reducing latency for data-ready threads.
// CPU_BOUND threads are dispatched to the back and subject to preemption.

enum
{
    APTH_CLASS_DEFAULT = 0,   // Normal FIFO scheduling
#define APTH_CLASS_DEFAULT APTH_CLASS_DEFAULT
    APTH_CLASS_IO_BOUND,      // Prioritize on wake (e.g., JVM mutators with RDMA)
#define APTH_CLASS_IO_BOUND APTH_CLASS_IO_BOUND
    APTH_CLASS_CPU_BOUND,     // Subject to preemption (e.g., GC workers, JIT compiler)
#define APTH_CLASS_CPU_BOUND APTH_CLASS_CPU_BOUND
    APTH_CLASS_REALTIME,      // Pin to specific scheduler, minimal yield
#define APTH_CLASS_REALTIME APTH_CLASS_REALTIME
    APTH_CLASS_DEDICATED,     // 1:1 dedicated pthread, bypasses scheduler
#define APTH_CLASS_DEDICATED APTH_CLASS_DEDICATED
    APTH_CLASS_DISTRIBUTED    // Round-robin across schedulers (e.g., GC workers)
#define APTH_CLASS_DISTRIBUTED APTH_CLASS_DISTRIBUTED
};

int apth_attr_setclass_np(apth_attr_t *attr, int thread_class);
int apth_attr_getclass_np(const apth_attr_t *attr, int *thread_class);

// ==================== Thread Attributes ====================

enum
{
    APTH_CREATE_JOINABLE,
#define APTH_CREATE_JOINABLE APTH_CREATE_JOINABLE
    APTH_CREATE_DETACHED
#define APTH_CREATE_DETACHED APTH_CREATE_DETACHED
};

/* Scheduler inheritance.  */
enum
{
    APTH_INHERIT_SCHED,
#define APTH_INHERIT_SCHED APTH_INHERIT_SCHED
    APTH_EXPLICIT_SCHED
#define APTH_EXPLICIT_SCHED APTH_EXPLICIT_SCHED
};

/* Scope handling.  */
enum
{
    APTH_SCOPE_SYSTEM,
#define APTH_SCOPE_SYSTEM APTH_SCOPE_SYSTEM
    APTH_SCOPE_PROCESS
#define APTH_SCOPE_PROCESS APTH_SCOPE_PROCESS
};

typedef unsigned int apth_key_t;

/* Once-only execution.
 * _Atomic(int) is C11; for C++ we use volatile int which is ABI-compatible.
 * apth_once() uses __atomic builtins internally, not the type's atomicity. */
#ifdef __cplusplus
typedef volatile int apth_once_t;
#else
typedef _Atomic(int) apth_once_t;
#endif
#define APTH_ONCE_INIT 0

/* For C++ callers: apth_func_raw(name) resolves to name directly.
 * LIBAPTH hooks only interpose when loaded via LD_PRELOAD;
 * when statically linked, libc functions are not hooked. */
#ifdef __cplusplus
#ifndef apth_func_raw
#define apth_func_raw(name) name
#endif
#endif

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
#ifndef __cplusplus
/* Syscall hook declarations — C only (conflict with C++ overloads/keywords) */
#include <sys/socket.h>              // for socklen_t
#include <sys/poll.h>                // for nfds_t
#include <bits/types/struct_iovec.h> // for struct iovec
#include <bits/types/stack_t.h>
#include <bits/types/struct_sigstack.h>
#include <bits/types/struct_rusage.h>
#include <signal.h> // for sigaction

#define APTH_EXPOSE_DECLARE_SYSCALL(rettype, name, ...) extern rettype name(__VA_ARGS__);

// ==================== Low Level I/O ====================
APTH_EXPOSE_DECLARE_SYSCALL(int, open, const char *pathname, int flags, ... /* mode_t mode */)
APTH_EXPOSE_DECLARE_SYSCALL(int, open64, const char *pathname, int flags, ... /* mode_t mode */)
APTH_EXPOSE_DECLARE_SYSCALL(int, openat, int dirfd, const char *pathname, int flags, ... /* mode_t mode */)
APTH_EXPOSE_DECLARE_SYSCALL(int, openat64, int filedes, const char *filename, int flags, ... /* mode_t mode */)
APTH_EXPOSE_DECLARE_SYSCALL(int, creat, const char *pathname, mode_t mode)
APTH_EXPOSE_DECLARE_SYSCALL(int, creat64, const char *filename, mode_t mode)
APTH_EXPOSE_DECLARE_SYSCALL(int, close, int fd)
APTH_EXPOSE_DECLARE_SYSCALL(int, close_range, unsigned int lowfd, unsigned int maxfd, int flags)
APTH_EXPOSE_DECLARE_SYSCALL(void, closefrom, int lowfd)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, read, int fd, void *buf, size_t nbytes)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, __read_chk, int fd, void *buf, size_t nbytes, size_t buflen)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, pread, int fd, void *buf, size_t count, off_t offset)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, __pread_chk, int fd, void *buf, size_t nbytes, off_t offset, size_t buflen)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, pread64, int fd, void *buf, size_t count, off64_t offset)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, __pread64_chk, int fd, void *buf, size_t nbytes, off64_t offset, size_t buflen)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, write, int fd, const void *buf, size_t nbytes)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, pwrite, int fd, const void *buf, size_t count, off_t offset)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, pwrite64, int filedes, const void *buffer, size_t size, off64_t offset)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, readv, int fd, const struct iovec *iov, int iovcnt)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, writev, int fd, const struct iovec *iov, int iovcnt)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, preadv, int fd, const struct iovec *iov, int iovcnt, off_t offset)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, preadv64, int fd, const struct iovec *iov, int iovcnt, off64_t offset)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, pwritev, int fd, const struct iovec *iov, int iovcnt, off_t offset)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, pwritev64, int fd, const struct iovec *iov, int iovcnt, off64_t offset)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, preadv2, int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, preadv64v2, int fd, const struct iovec *iov, int iovcnt, off64_t offset, int flags)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, pwritev2, int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, pwritev64v2, int fd, const struct iovec *iov, int iovcnt, off64_t offset, int flags)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, copy_file_range, int inputfd, off64_t *inputpos, int outputfd,
                            off64_t *outputpos, size_t length, unsigned int flags /* must be zero */)
APTH_EXPOSE_DECLARE_SYSCALL(int, dup, int old)
APTH_EXPOSE_DECLARE_SYSCALL(int, dup2, int old, int newfd)
APTH_EXPOSE_DECLARE_SYSCALL(int, dup3, int old, int newfd, int flags)
APTH_EXPOSE_DECLARE_SYSCALL(int, select, int nfd, fd_set *rfds, fd_set *wfds,
                            fd_set *efds, struct timeval *timeout)
APTH_EXPOSE_DECLARE_SYSCALL(int, pselect, int nfds, fd_set *rfds, fd_set *wfds,
                            fd_set *efds, const struct timespec *ts, const sigset_t *mask)
APTH_EXPOSE_DECLARE_SYSCALL(int, poll, struct pollfd *fds, nfds_t nfds, int timeout)
APTH_EXPOSE_DECLARE_SYSCALL(int, pipe, int pipefd[2])
APTH_EXPOSE_DECLARE_SYSCALL(int, fcntl, int fd, int cmd, ... /* arg */)

// ==================== Process ====================
APTH_EXPOSE_DECLARE_SYSCALL(int, system, const char *cmd)
APTH_EXPOSE_DECLARE_SYSCALL(pid_t, fork, void)
APTH_EXPOSE_DECLARE_SYSCALL(pid_t, _Fork, void)
APTH_EXPOSE_DECLARE_SYSCALL(pid_t, vfork, void)
APTH_EXPOSE_DECLARE_SYSCALL(pid_t, waitpid, pid_t wpid, int *status, int options)
APTH_EXPOSE_DECLARE_SYSCALL(pid_t, wait, int *status_ptr)
APTH_EXPOSE_DECLARE_SYSCALL(pid_t, wait4, pid_t pid, int *status_ptr, int options, struct rusage *usage)
APTH_EXPOSE_DECLARE_SYSCALL(void, exit, int status)

// ==================== Signal ====================
typedef void (*sighandler_t)(int);
APTH_EXPOSE_DECLARE_SYSCALL(sighandler_t, signal, int sig, sighandler_t handler)
APTH_EXPOSE_DECLARE_SYSCALL(sighandler_t, sysv_signal, int sig, sighandler_t handler)
APTH_EXPOSE_DECLARE_SYSCALL(sighandler_t, __sysv_signal, int sig, sighandler_t handler)
APTH_EXPOSE_DECLARE_SYSCALL(int, sigaction, int signum, const struct sigaction *restrict act,
                            struct sigaction *restrict oldact)
APTH_EXPOSE_DECLARE_SYSCALL(int, sigwait, const sigset_t *set, int *sigp)
APTH_EXPOSE_DECLARE_SYSCALL(int, raise, int sig)
APTH_EXPOSE_DECLARE_SYSCALL(int, gsignal, int signum)
APTH_EXPOSE_DECLARE_SYSCALL(int, sigprocmask, int how, const sigset_t *restrict set,
                            sigset_t *restrict oldset)
APTH_EXPOSE_DECLARE_SYSCALL(int, sigpending, sigset_t *set)
APTH_EXPOSE_DECLARE_SYSCALL(int, pause, void)
APTH_EXPOSE_DECLARE_SYSCALL(int, sigsuspend, const sigset_t *mask)
APTH_EXPOSE_DECLARE_SYSCALL(int, sigaltstack, const stack_t *restrict ss, stack_t *restrict oss)
APTH_EXPOSE_DECLARE_SYSCALL(int, sigstack, struct sigstack *stack, struct sigstack *oldstack)

// ==================== Socket ====================
APTH_EXPOSE_DECLARE_SYSCALL(int, socket, int domain, int type, int protocol)
APTH_EXPOSE_DECLARE_SYSCALL(int, shutdown, int fd, int how)
APTH_EXPOSE_DECLARE_SYSCALL(int, socketpair, int domain, int style, int protocol, int filedes[2])
APTH_EXPOSE_DECLARE_SYSCALL(int, connect, int fd, const struct sockaddr *address, socklen_t address_len)
APTH_EXPOSE_DECLARE_SYSCALL(int, accept, int fd, struct sockaddr *addr, socklen_t *addrlen)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, recv, int sockfd, void *buf, size_t len, int flags)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, __recv_chk, int sockfd, void *buf, size_t len, size_t buflen, int flags)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, send, int sockfd, const void *buf, size_t len, int flags)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, recvfrom, int sockfd, void *buf, size_t nbytes,
                            int flags, struct sockaddr *src_addr, socklen_t *addrlen)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, __recvfrom_chk, int __fd, void *__restrict __buf, size_t __n,
                            size_t __buflen, int __flags, struct sockaddr *__addr,
                            socklen_t *__restrict __addr_len)
APTH_EXPOSE_DECLARE_SYSCALL(ssize_t, sendto, int sockfd, const void *buf, size_t nbytes,
                            int flags, const struct sockaddr *dest_addr, socklen_t dest_len)

// ==================== Time ====================
APTH_EXPOSE_DECLARE_SYSCALL(int, nanosleep, const struct timespec *rqtp, struct timespec *rmtp)
APTH_EXPOSE_DECLARE_SYSCALL(int, usleep, unsigned int usec)
APTH_EXPOSE_DECLARE_SYSCALL(unsigned int, sleep, unsigned int sec)
#undef APTH_EXPOSE_DECLARE_SYSCALL
#endif /* !__cplusplus — end of syscall hook declarations */

// ==================== Thread Inspection ====================
// Public state constants (for use with apth_getstate).
#define APTH_THREAD_STATE_NEW         0x02
#define APTH_THREAD_STATE_READY       0x04
#define APTH_THREAD_STATE_WAITING     0x08
#define APTH_THREAD_STATE_TERMINATED  0x10
#define APTH_THREAD_STATE_WAKED       0x20
#define APTH_THREAD_STATE_RUNNING     0x40

// Query the current scheduling state of a thread.
// Returns one of the APTH_THREAD_STATE_* constants, or -1 if th is invalid.
int apth_getstate(apth_t th);

// Get the saved stack pointer of a yielded thread (for GC stack walking).
// Only valid when apth_getstate(th) != APTH_THREAD_STATE_RUNNING.
// Returns 0 on success, ESRCH if invalid, EBUSY if thread is RUNNING,
// ENOTSUP for dedicated threads (they use pthread stack).
int apth_get_saved_sp(apth_t th, void **sp_out);

// Get the stack memory bounds of a thread.
// base_out: start of usable stack memory (after guard page).
// size_out: usable stack size in bytes.
// Returns 0 on success, ESRCH if invalid, ENOTSUP for dedicated threads.
int apth_get_stack_bounds(apth_t th, void **base_out, size_t *size_out);

// Thread statistics.
struct apth_thread_stats {
    int dispatches;
    double cpu_time_sec;
    double wall_time_sec;
    int thread_class;
    int state;
};

int apth_get_thread_stats(apth_t th, struct apth_thread_stats *stats);

// ==================== Thread Enumeration ====================
// Visitor callback for apth_for_each_thread.
// Return 0 to continue iteration, non-zero to stop early.
typedef int (*apth_thread_visitor_t)(apth_t th, void *arg);

// Iterate all live threads across all schedulers and dedicated threads.
// Calls visitor(th, arg) for each thread. Returns count of threads visited.
int apth_for_each_thread(apth_thread_visitor_t visitor, void *arg);

// Return the number of worker pthreads (schedulers).
int apth_get_worker_count(void);

// ==================== Safepoint Cooperation ====================
// Request all schedulers to pause (stop dispatching threads).
// Blocks until no scheduler has a RUNNING thread.
int apth_request_pause_all(void);

// Resume all schedulers after a pause.
int apth_resume_all(void);

// State-change callback: fired when a thread transitions between states.
typedef void (*apth_state_callback_t)(apth_t th, int old_state, int new_state, void *arg);
int apth_set_state_callback(apth_state_callback_t cb, void *arg);

// Preemption hook: fired from apth_preempt_check() before yielding.
typedef void (*apth_preempt_hook_t)(apth_t th, void *arg);
int apth_set_preempt_hook(apth_preempt_hook_t hook, void *arg);

/* ==================== Dedicated Thread Attachment ==================== */
int apth_attach_self_as_dedicated(apth_t *out);
int apth_detach_self(void);
int apth_prevent_dispatch(apth_t t);
int apth_allow_dispatch(apth_t t);

// ==================== Functions ====================

#include <stdbool.h>
#include <sched.h>                // For cpu_set_t
#include <bits/types/clockid_t.h> // For clockid_t

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
int apth_yield_optional(void);
int apth_kill(apth_t t, int sig);
int apth_equal(apth_t t1, apth_t t2);
int apth_sigmask(int how, const sigset_t *set, sigset_t *oldset);
void apth_testcancel(void);
int apth_once(apth_once_t *once_control, void (*init_routine)(void));
size_t apth_get_minstack(const apth_attr_t *attr);
int apth_getcpuclockid(apth_t th, clockid_t *clock_id);
int apth_setcpuclockid(apth_t th, clockid_t clock_id);
int apth_getattr_np(apth_t th, apth_attr_t *attr);

int apth_attr_init(apth_attr_t *attr);
int apth_attr_destroy(apth_attr_t *attr);
int apth_attr_getname_np(apth_attr_t *attr, char *buf, size_t len);
int apth_attr_setname_np(apth_attr_t *attr, const char *name);
int apth_attr_getdetachstate(const apth_attr_t *attr, int *detachstate);
int apth_attr_setdetachstate(apth_attr_t *attr, int detachstate);
int apth_attr_getaffinity_np(const apth_attr_t *attr, size_t cpusetsize, cpu_set_t *cpuset);
int apth_attr_setaffinity_np(apth_attr_t *attr, size_t cpusetsize, const cpu_set_t *cpuset);
int apth_attr_getsigmask_np(const apth_attr_t *attr, sigset_t *sigmask);
int apth_attr_setsigmask_np(apth_attr_t *attr, const sigset_t *sigmask);
int apth_attr_getstackaddr(const apth_attr_t *attr, void **stackaddr);
int apth_attr_setstackaddr(apth_attr_t *attr, void *stackaddr);
int apth_attr_getstacksize(const apth_attr_t *attr, size_t *stacksize);
int apth_attr_setstacksize(apth_attr_t *attr, size_t stacksize);
int apth_attr_getstack(const apth_attr_t *attr, void **stackaddr, size_t *stacksize);
int apth_attr_setstack(apth_attr_t *attr, void *stackaddr, size_t stacksize);
int apth_attr_getguardsize(const apth_attr_t *attr, size_t *guardsize);
int apth_attr_setguardsize(apth_attr_t *attr, size_t guardsize);

int apth_attr_getscope(const apth_attr_t *attr, int *scope);
int apth_attr_setscope(apth_attr_t *attr, int scope);
int apth_attr_getschedpolicy(const apth_attr_t *attr, int *policy);
int apth_attr_setschedpolicy(apth_attr_t *attr, int policy);
int apth_attr_getschedparam(const apth_attr_t *attr, struct sched_param *param);
int apth_attr_setschedparam(apth_attr_t *attr, const struct sched_param *param);

// ==================== Synchronization Primitives ====================

// --- Mutex ---

typedef union
{
    char __size[__SIZEOF_APTH_MUTEX_T];
    long int __align;
} apth_mutex_t;

typedef union
{
    char __size[__SIZEOF_APTH_MUTEXATTR_T];
    int __align;
} apth_mutexattr_t;

enum
{
    APTH_MUTEX_NORMAL = 0,
#define APTH_MUTEX_NORMAL APTH_MUTEX_NORMAL
    APTH_MUTEX_ERRORCHECK,
#define APTH_MUTEX_ERRORCHECK APTH_MUTEX_ERRORCHECK
    APTH_MUTEX_RECURSIVE,
#define APTH_MUTEX_RECURSIVE APTH_MUTEX_RECURSIVE
    APTH_MUTEX_DEFAULT = APTH_MUTEX_NORMAL
#define APTH_MUTEX_DEFAULT APTH_MUTEX_DEFAULT
};

// Mutex static initializers
#define APTH_MUTEX_INITIALIZER {{0}}
#define APTH_RECURSIVE_MUTEX_INITIALIZER_NP {{0}}
#define APTH_ERRORCHECK_MUTEX_INITIALIZER_NP {{0}}
#define APTH_ADAPTIVE_MUTEX_INITIALIZER_NP {{0}}

int apth_mutexattr_init(apth_mutexattr_t *attr);
int apth_mutexattr_destroy(apth_mutexattr_t *attr);
int apth_mutexattr_gettype(const apth_mutexattr_t *attr, int *type);
int apth_mutexattr_settype(apth_mutexattr_t *attr, int type);

int apth_mutex_init(apth_mutex_t *mutex, const apth_mutexattr_t *attr);
int apth_mutex_destroy(apth_mutex_t *mutex);
int apth_mutex_lock(apth_mutex_t *mutex);
int apth_mutex_timedlock(apth_mutex_t *mutex, const struct timespec *abstime);
int apth_mutex_trylock(apth_mutex_t *mutex);
int apth_mutex_unlock(apth_mutex_t *mutex);

// --- Condition Variable ---

typedef union
{
    char __size[__SIZEOF_APTH_COND_T];
    long long int __align;
} apth_cond_t;

typedef union
{
    char __size[__SIZEOF_APTH_CONDATTR_T];
    int __align;
} apth_condattr_t;

// Condition variable static initializer
#define APTH_COND_INITIALIZER {{0}}

int apth_condattr_init(apth_condattr_t *attr);
int apth_condattr_destroy(apth_condattr_t *attr);
int apth_condattr_setclock(apth_condattr_t *attr, clockid_t clock_id);
int apth_condattr_getclock(const apth_condattr_t *attr, clockid_t *clock_id);

int apth_cond_init(apth_cond_t *cond, const apth_condattr_t *attr);
int apth_cond_destroy(apth_cond_t *cond);
int apth_cond_wait(apth_cond_t *cond, apth_mutex_t *mutex);
int apth_cond_timedwait(apth_cond_t *cond, apth_mutex_t *mutex, const struct timespec *abstime);
int apth_cond_signal(apth_cond_t *cond);
int apth_cond_broadcast(apth_cond_t *cond);

// --- Barrier ---

typedef union
{
    char __size[__SIZEOF_APTH_BARRIER_T];
    long int __align;
} apth_barrier_t;

#define APTH_BARRIER_SERIAL_THREAD (-1)

// Barrier static initializer (note: barriers typically require init with count)
#define APTH_BARRIER_INITIALIZER {{0}}

int apth_barrier_init(apth_barrier_t *barrier, const void *attr, unsigned int count);
int apth_barrier_destroy(apth_barrier_t *barrier);
int apth_barrier_wait(apth_barrier_t *barrier);

// --- Semaphore ---

typedef union
{
    char __size[__SIZEOF_APTH_SEM_T];
    long int __align;
} apth_sem_t;

// Semaphore static initializer (note: semaphores typically require init with value)
#define APTH_SEM_INITIALIZER {{0}}

int apth_sem_init(apth_sem_t *sem, int pshared, unsigned int value);
int apth_sem_destroy(apth_sem_t *sem);
int apth_sem_wait(apth_sem_t *sem);
int apth_sem_timedwait(apth_sem_t *sem, const struct timespec *abstime);
int apth_sem_trywait(apth_sem_t *sem);
int apth_sem_post(apth_sem_t *sem);
int apth_sem_getvalue(apth_sem_t *sem, int *sval);

// --- Read-Write Lock ---

typedef union
{
    char __size[__SIZEOF_APTH_RWLOCK_T];
    long int __align;
} apth_rwlock_t;

// Read-write lock static initializer
#define APTH_RWLOCK_INITIALIZER {{0}}

int apth_rwlock_init(apth_rwlock_t *rwlock, const void *attr);
int apth_rwlock_destroy(apth_rwlock_t *rwlock);
int apth_rwlock_rdlock(apth_rwlock_t *rwlock);
int apth_rwlock_timedrdlock(apth_rwlock_t *rwlock, const struct timespec *abstime);
int apth_rwlock_tryrdlock(apth_rwlock_t *rwlock);
int apth_rwlock_wrlock(apth_rwlock_t *rwlock);
int apth_rwlock_timedwrlock(apth_rwlock_t *rwlock, const struct timespec *abstime);
int apth_rwlock_trywrlock(apth_rwlock_t *rwlock);
int apth_rwlock_unlock(apth_rwlock_t *rwlock);

// ==================== NUMA Topology ====================
// Available when compiled with -DAPTH_NUMA.
// On non-NUMA builds, stub implementations return sensible defaults.
#ifdef APTH_NUMA
// Return the number of NUMA nodes detected on the system.
// Always >= 1 (1 means UMA / single-node).
int apth_get_numa_node_count(void);

// Return the NUMA node that thread th is currently scheduled on.
// Returns -1 if th is a dedicated thread (no scheduler) or invalid.
int apth_get_thread_numa_node(apth_t th);
#else
// Stubs for non-NUMA builds: always report single node.
static inline int apth_get_numa_node_count(void) { return 1; }
static inline int apth_get_thread_numa_node(apth_t th) { (void)th; return 0; }
#endif

// ==================== RDMA Completion Waiting ====================
// Available when compiled with -DAPTH_USE_RDMA and linked with -libverbs.
// See docs/JVM_RDMA_INTEGRATION.md for usage guide.
#ifdef APTH_USE_RDMA
#include <stdint.h>
struct ibv_cq;
struct ibv_wc;
int apth_rdma_register_cq(struct ibv_cq *cq);
void apth_rdma_unregister_cq(struct ibv_cq *cq);
int apth_rdma_wait(struct ibv_cq *cq, uint64_t wr_id, struct ibv_wc *wc);
int apth_rdma_wait_batch(struct ibv_cq *cq, uint64_t *wr_ids, int count, struct ibv_wc *wcs);
#endif

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

extern struct __apth_main_args __LIBAPTH_MAIN_ARGS;

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
#define APTH_MAIN_BEGIN(argc_name, argv_name)                      \
    static void *__apth_main_impl__(void *__apth_args__);          \
    int main(int __apth_argc__, char *__apth_argv__[])             \
    {                                                              \
        struct __apth_main_args *__margs__ = &__LIBAPTH_MAIN_ARGS; \
        __margs__->argc = __apth_argc__;                           \
        __margs__->argv = __apth_argv__;                           \
        apth_init_t __initvals__;                                  \
        apth_configure(&__initvals__);                             \
        __initvals__.main_apth = __apth_main_impl__;               \
        __initvals__.main_args = (void *)__margs__;                \
        apth_init(&__initvals__);                                  \
        return -1;                                                 \
    }                                                              \
    static void *__apth_main_impl__(void *__apth_args__)           \
    {                                                              \
        int argc_name =                                            \
            ((struct __apth_main_args *)__apth_args__)->argc;      \
        char **argv_name =                                         \
            ((struct __apth_main_args *)__apth_args__)->argv;      \
        (void)argc_name;                                           \
        (void)argv_name;                                           \
        if (1)

/* Closes the function body opened by APTH_MAIN_BEGIN. */
#define APTH_MAIN_END \
    return (void *)0; \
    }

#ifdef __cplusplus
}
#endif

#endif /* __LIBAPTH_H */
