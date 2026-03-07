#ifndef __LIBAPTH_INTERNAL_APTH_SCHED_H
#define __LIBAPTH_INTERNAL_APTH_SCHED_H

#include "apth.h"
#include "internal/forward_declare.h"
#include "internal/apth_ctx.h"
#include "internal/apth_time.h"
#include "internal/apth_fd_slot.h"
#include "utils/lll_new.h"

// Per-thread scheduler. Note that we do not treat scheduler as a separated
// thread but a background role. Besides, since the main thread only runs on
// one of schedulers, holding a special reference field to the main thread is
// meaningless here.
// TODO: actually, only `ready_list_lock` is really needed. Since work stealing
// will only occurs at ready list.
struct apth_perpthr_scheduler
{
    sched_id id;                     // scheduler ID
    apth_cxt_t sched_ctx;            // scheduler context (as trampoline)
    apth_thqueue_t new_queue;        // new threads
    apth_thqueue_t ready_queue;      // threads ready to run
    apth_thqueue_t waiting_queue;    // threads waiting for an event
    apth_thqueue_t terminated_queue; // terminated threads
    apth_thqueue_t waked_queue;      // threads waked by event(s)
    apth_thqueue_t running_queue;    // should assert size == 1
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

APTH_INTERNAL void sched_key_t_init(void);
APTH_INTERNAL void sched_key_t_drop(void);

APTH_INTERNAL apth_t get_MAIN_APTH(void);
APTH_INTERNAL apth_t *get_addr_of_MAIN_APTH(void);
APTH_INTERNAL void set_MAIN_APTH(apth_t main_th);

APTH_INTERNAL bool apth_scheduler_init(apth_sched_t sched, apth_worker_t worker);
APTH_INTERNAL void apth_scheduler_kill(void);
APTH_INTERNAL void inc_thrcnt(apth_sched_t sched);
APTH_INTERNAL void dec_thrcnt(apth_sched_t sched);
APTH_INTERNAL unsigned int get_apth_nthreads(void);
APTH_INTERNAL void inc_alive_thrcnt(void);
APTH_INTERNAL void dec_alive_thrcnt(void);
APTH_INTERNAL unsigned int get_apth_alive_nthreads(void);

APTH_INTERNAL bool apth_sched_is_opening(apth_sched_t sched);
APTH_INTERNAL void apth_sched_calc_load(apth_sched_t sched, apth_time_t *now);
APTH_INTERNAL void apth_sched_wake(apth_sched_t sched);
APTH_INTERNAL void *scheduler_routine(void *arg);

// INLINE_ALWAYS apth_sched_t cur_sched(void)
// {
// #ifdef APTH_CUR_USING_KEYWORD
//     return __cur_sched_tls;
// #else
//     return (apth_sched_t)apth_func_raw(pthread_getspecific)(__CUR_SCHED_KEY);
// #endif
// }

// INLINE_ALWAYS void set_cur_sched(apth_sched_t sched)
// {
// #ifdef APTH_CUR_USING_KEYWORD
//     __cur_sched_tls = sched;
// #else
//     int result = apth_func_raw(pthread_setspecific)(__CUR_SCHED_KEY, sched);
//     assert_msg(result == 0, "fail pthread_setspecific result = %d (%s)", result, strerror(result));
// #endif
// }

#endif // __LIBAPTH_INTERNAL_APTH_SCHED_H
