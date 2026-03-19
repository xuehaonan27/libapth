#ifndef __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_SCHED_ST_H
#define __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_SCHED_ST_H

#include "apth.h"
#include "internal/forward_declare.h"
#include "internal/apth_ctx.h"
#include "internal/apth_time.h"
#include "internal/apth_thqueue.h"
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

    // Scheduler's own epoll_fd: only monitors wake_eventfd for blocking.
    // FD I/O events are handled by the global reactor.
    int epoll_fd;
    int wake_eventfd;                // eventfd used to wake the scheduler

    // Stack memory pool: reuse mmap'd stacks from dead threads to avoid
    // expensive mmap/munmap syscalls (TLB shootdowns) on thread lifecycle.
#define APTH_STACK_POOL_MAX 32
    struct {
        char *mem;      // mmap'd region start
        size_t size;    // total size (stacksize + guardsize)
    } stack_pool[APTH_STACK_POOL_MAX];
    int stack_pool_count;
};

#endif // __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_SCHED_ST_H
