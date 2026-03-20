#ifndef __LIBAPTH_INTERNAL_APTH_REACTOR_H
#define __LIBAPTH_INTERNAL_APTH_REACTOR_H

#include "apth.h"
#include "internal/forward_declare.h"
#include "internal/apth_fd_slot.h"
#include "internal/apth_epoll_waiter.h"
#include "utils/lll.h"
#include "utils/list.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef APTH_USE_REACTOR

// Request types for scheduler -> reactor communication
enum apth_reactor_req_type
{
    REACTOR_REQ_ADD_WAITER,    // Register FD event with epoll
    REACTOR_REQ_REMOVE_WAITER, // Cancel pending FD event
    REACTOR_REQ_FD_CLOSED,     // Fail all waiters for fd
};

struct apth_reactor_request
{
    enum apth_reactor_req_type type;
    int fd;
    apth_t th;
    apth_event_t ev;
    apth_sched_t sched; // Target scheduler for wakeups
    struct list_elem elem;
};

// Request pool size (pre-allocated to avoid malloc in hot path)
#define APTH_REACTOR_REQ_POOL_SIZE 512

struct apth_reactor
{
    int epoll_fd;
    int wake_eventfd;
    pthread_t thread;
    _Atomic(bool) running;

    // Dynamic fd slot table (replaces per-sched fd_slot_table)
    struct apth_epoll_fd_slot *fd_slots;
    int fd_slot_capacity;
    struct list active_fd_slots;
    int active_fd_count;

    // Waiter pool
    struct apth_epoll_waiter *waiter_pool;
    struct list free_waiters;
    int waiter_pool_size;
    int waiter_pool_allocated;

    // Request queue (MPSC: schedulers -> reactor)
    lll_internal_t req_lock;
    struct list req_queue;

    // Request pool (pre-allocated)
    struct apth_reactor_request req_pool[APTH_REACTOR_REQ_POOL_SIZE];
    struct list free_reqs;
    lll_internal_t req_pool_lock;

    // Pending fd close
#define APTH_REACTOR_PENDING_FD_CLOSE_MAX 256
    int pending_fd_close_fds[APTH_REACTOR_PENDING_FD_CLOSE_MAX];
    _Atomic(int) pending_fd_close_count;
    lll_internal_t pending_fd_close_lock;
};

#ifdef APTH_NUMA
// One reactor per NUMA node (stub: currently only 1 node)
extern struct apth_reactor *APTH_REACTORS;
extern int APTH_REACTOR_COUNT;
#define REACTOR_FOR(sched) (&APTH_REACTORS[(sched)->numa_node])
// Default reactor (node 0) for contexts without a scheduler
#define APTH_DEFAULT_REACTOR (&APTH_REACTORS[0])
#else
// Single global reactor
extern struct apth_reactor APTH_GLOBAL_REACTOR;
#define REACTOR_FOR(sched) (&APTH_GLOBAL_REACTOR)
#define APTH_DEFAULT_REACTOR (&APTH_GLOBAL_REACTOR)
#endif

APTH_INTERNAL int apth_reactor_init(void);
APTH_INTERNAL void apth_reactor_destroy(void);

// Submit requests from scheduler context (thread-safe, MPSC)
APTH_INTERNAL int apth_reactor_add_waiter(apth_sched_t sched, int fd, apth_t th, apth_event_t ev);
APTH_INTERNAL void apth_reactor_remove_waiter(apth_sched_t sched, int fd, apth_t th, apth_event_t ev);
APTH_INTERNAL void apth_reactor_notify_fd_closed(int fd);

#endif // APTH_USE_REACTOR

#endif // __LIBAPTH_INTERNAL_APTH_REACTOR_H
