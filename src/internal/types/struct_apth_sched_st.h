#ifndef __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_SCHED_ST_H
#define __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_SCHED_ST_H

#include "apth.h"
#include "internal/forward_declare.h"
#include "internal/apth_ctx.h"
#include "internal/apth_time.h"
#include "internal/apth_thqueue.h"
#include "internal/apth_fd_slot.h"
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
    struct apth_thqueue_st running_queue;    // should assert size == 1
#define THQUEUE(SCHED, NAME) ((apth_thqueue_t)(&(SCHED)->NAME##_queue))
    apth_worker_t worker;            // pthread worker carrying this scheduler
    unsigned int switches;           // context switch times
    _Atomic(unsigned int) thrcnt;    // APTH threads now running on this scheduler
    apth_time_t running;             // time the scheduler runs
    apth_t cur;                      // current APTH
    _Atomic(apth_t) advised_next_th; // advised thread to run next
    volatile _Atomic(bool) opening;  // scheduler is opening
    apth_time_t apth_loadticknext;   // scheduler load next tick
    float loadval;                   // scheduler load value
    int epoll_fd;                    // epoll instance for this scheduler
    int wake_eventfd;                // eventfd used to wake the scheduler from epoll_wait

    struct apth_epoll_fd_slot fd_slot_table[APTH_EPOLL_FD_SLOT_TABLE_SIZE]; // fd -> slot fast search
    struct list active_fd_slots;                                            // slots with waiters
    int active_fd_count;
    struct list dirty_fd_slots;                                             // slots needing epoll MOD

// Memory pool for apth_epoll_waiter structures to avoid malloc/free
#define APTH_WAITER_POOL_SIZE 256
    struct apth_epoll_waiter waiter_pool[APTH_WAITER_POOL_SIZE];
    struct list free_waiters;  // List of free waiter structures
    int waiter_pool_allocated; // Number of waiters allocated from pool

    // Pending fd close notifications from other schedulers (or self).
    // When an fd is closed, the closing scheduler pushes the fd number into
    // every scheduler's pending list. Each scheduler drains its list at the
    // beginning of its event manager loop, failing all local waiters for
    // those fds.
#define APTH_PENDING_FD_CLOSE_MAX 128
    int pending_fd_close_fds[APTH_PENDING_FD_CLOSE_MAX];
    _Atomic(int) pending_fd_close_count;
    lll_internal_t pending_fd_close_lock; // Type 2 LLL
};

#endif // __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_SCHED_ST_H
