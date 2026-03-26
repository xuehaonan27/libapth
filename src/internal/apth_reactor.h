#ifndef __LIBAPTH_INTERNAL_APTH_REACTOR_H
#define __LIBAPTH_INTERNAL_APTH_REACTOR_H

/*
 * Global I/O Reactor
 *
 * A dedicated pthread that owns a single epoll instance for all FD I/O.
 * Schedulers submit watch/unwatch requests via a lock-protected queue;
 * the reactor runs epoll_wait and wakes schedulers when FDs become ready.
 *
 * This decouples I/O polling from scheduling, so the scheduler loop can
 * dispatch threads without any epoll_wait syscall on the fast path.
 *
 * When io_uring is active on a scheduler, that scheduler handles its own
 * I/O directly (no reactor involvement).  The reactor is the epoll-only
 * backend.
 */

#include "apth.h"
#include "internal/forward_declare.h"
#include "internal/apth_fd_slot.h"
#include "internal/apth_epoll_waiter.h"
#include "internal/types/struct_apth_event_st.h"
#include "utils/archplattoold.h"
#include "utils/lll.h"
#include "utils/list.h"
#include <stdbool.h>
#include <stdint.h>

/* Request types submitted by schedulers to the reactor. */
enum reactor_req_type
{
    REACTOR_REQ_WATCH,    /* Register FD for readiness notification */
    REACTOR_REQ_UNWATCH,  /* Remove a specific waiter for an FD */
    REACTOR_REQ_FD_CLOSE, /* FD is being closed — fail all waiters */
};

/* A single request from a scheduler to the reactor. */
struct reactor_request
{
    enum reactor_req_type type;
    int fd;
    apth_goal_t goal;         /* APTH_GOAL_UNTIL_FD_READABLE etc (for WATCH) */
    apth_event_t ev;          /* Event to mark OCCURRED (for WATCH) */
    apth_t th;                /* Thread owning the event */
    apth_sched_t sched;       /* Scheduler to wake on completion */
};

#define REACTOR_REQ_QUEUE_SIZE 4096

/* Global reactor state. */
struct apth_reactor
{
    pthread_t thread;
    int epoll_fd;
    int wake_fd;              /* eventfd: wakes reactor from epoll_wait */
    _Atomic(bool) running;

    /* Request queue: schedulers push, reactor drains.
     * Protected by req_lock.  Simple bounded buffer. */
    lll_internal_t req_lock;
    struct reactor_request reqs[REACTOR_REQ_QUEUE_SIZE];
    int req_count;

    /* FD slot table (global, owned by reactor thread) */
    struct apth_epoll_fd_slot *fd_slots;
    int fd_slot_capacity;
    struct list active_fd_slots;
    int active_fd_count;

    /* Waiter pool (owned by reactor thread) */
#define REACTOR_WAITER_POOL_SIZE 1024
    struct apth_epoll_waiter *waiter_pool;
    struct list free_waiters;
    int waiter_pool_size;
    int waiter_pool_allocated;
};

/* The global reactor instance. */
extern struct apth_reactor GLOBAL_REACTOR;

/* Lifecycle */
APTH_INTERNAL int apth_reactor_start(void);
APTH_INTERNAL void apth_reactor_stop(void);

/* Submit a watch request.  Called from any thread context before yielding.
 * Thread-safe (lock-protected).  Returns 0 on success, -1 if full. */
APTH_INTERNAL int apth_reactor_submit_watch(int fd, apth_goal_t goal,
                                             apth_event_t ev, apth_t th,
                                             apth_sched_t sched);

/* Submit an unwatch request.  Thread-safe. */
APTH_INTERNAL void apth_reactor_submit_unwatch(int fd, apth_event_t ev);

/* Notify reactor that an FD is being closed. */
APTH_INTERNAL void apth_reactor_notify_fd_closed(int fd);

/* Check if the global reactor is active. */
INLINE bool apth_reactor_is_active(void)
{
    return __atomic_load_n(&GLOBAL_REACTOR.running, __ATOMIC_ACQUIRE);
}

#endif /* __LIBAPTH_INTERNAL_APTH_REACTOR_H */
