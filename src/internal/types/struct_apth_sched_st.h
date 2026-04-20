#ifndef __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_SCHED_ST_H
#define __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_SCHED_ST_H

#include "apth.h"
#include "internal/forward_declare.h"
#include "internal/apth_ctx.h"
#include "internal/apth_time.h"
#include "internal/apth_thqueue.h"
#include "internal/apth_fd_slot.h"
#include "internal/apth_epoll_waiter.h"
#include "internal/apth_iouring.h"
#include "utils/lll.h"

// Per-thread scheduler. Note that we do not treat scheduler as a separated
// thread but a background role. Besides, since the main thread only runs on
// one of schedulers, holding a special reference field to the main thread is
// meaningless here.
struct apth_sched_st
{
    sched_id id;                     // scheduler ID
    struct apth_cxt_st sched_ctx_st; // scheduler context (as trampoline)
#define SCHED_CTX(S) ((apth_cxt_t) & ((S)->sched_ctx_st))
    struct apth_thqueue_st new_queue;        // new threads
    struct apth_thqueue_st ready_queue;      // threads ready to run
    struct apth_thqueue_st waiting_queue;    // threads waiting for an event
    struct apth_thqueue_st terminated_queue; // terminated threads
    struct apth_thqueue_st waked_queue;      // threads waked by event(s)
#define THQUEUE(SCHED, NAME) ((apth_thqueue_t)(&(SCHED)->NAME##_queue))
    apth_worker_t worker;            // pthread worker carrying this scheduler
    unsigned int switches;           // context switch times
    unsigned int switches_since_poll; // dispatches since last I/O poll
    _Atomic(unsigned int) thrcnt;    // APTH threads now running on this scheduler
    apth_time_t running;             // time the scheduler runs
    apth_t cur;                      // current APTH
    _Atomic(apth_t) advised_next_th; // advised thread to run next
    volatile _Atomic(bool) opening;  // scheduler is opening
    // Scheduler's own epoll_fd: monitors wake_eventfd + all FDs.
    // Set to -1 when io_uring is active (epoll not used).
    int epoll_fd;
    int wake_eventfd;                // eventfd used to wake the scheduler (io_uring/legacy)
    volatile int wake_futex_val;     // futex-based wake (reactor path, faster than eventfd)

#ifdef APTH_NUMA
    int numa_node;                   // NUMA node this scheduler is bound to
#endif

#ifdef APTH_USE_IOURING
    struct apth_iouring_ctx uring_ctx;  // per-scheduler io_uring ring
    bool use_iouring;                   // runtime flag: true = io_uring, false = epoll
    struct list uring_pending_cancels;  // cancelled waiters awaiting CQE before free
#endif

    // Per-scheduler FD management (shared by epoll and io_uring backends)
    struct apth_epoll_fd_slot *fd_slots;
    int fd_slot_capacity;
    struct list active_fd_slots;
    int active_fd_count;

    // Waiter pool
#define APTH_WAITER_POOL_SIZE 256
    struct apth_epoll_waiter *waiter_pool;
    struct list free_waiters;
    int waiter_pool_size;
    int waiter_pool_allocated;

    // Pending fd close notifications
#define APTH_PENDING_FD_CLOSE_MAX 128
    int pending_fd_close_fds[APTH_PENDING_FD_CLOSE_MAX];
    _Atomic(int) pending_fd_close_count;
    lll_internal_t pending_fd_close_lock;

    // Stack memory pool: reuse mmap'd stacks from dead threads to avoid
    // expensive mmap/munmap syscalls (TLB shootdowns) on thread lifecycle.
#define APTH_STACK_POOL_MAX 64
    struct {
        char *mem;      // mmap'd region start
        size_t size;    // total size (stacksize + guardsize)
    } stack_pool[APTH_STACK_POOL_MAX];
    int stack_pool_count;
};

#endif // __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_SCHED_ST_H
