#ifndef __LIBAPTH_INTERNAL_FUNCS_H
#define __LIBAPTH_INTERNAL_FUNCS_H

#include "internal_types.h"
#include "utils/archplattoold.h"

APTH_INTERNAL apth_thqueue_t belonging_queue_of(apth_t th, const char *dbg_msg);
APTH_INTERNAL void set_belonging_queue_of(apth_t th, apth_thqueue_t q);
APTH_INTERNAL void thqueue_init(apth_thqueue_t *queue, apth_sched_t sched, apth_state_t state);
APTH_INTERNAL size_t thqueue_size(apth_thqueue_t queue);
APTH_INTERNAL void push_apth_to(apth_thqueue_t queue, apth_t th);
APTH_INTERNAL apth_t pop_apth_from(apth_thqueue_t queue);
APTH_INTERNAL bool apth_is_in(apth_thqueue_t queue, apth_t th);
APTH_INTERNAL void remove_apth_from(apth_thqueue_t queue, apth_t th);
APTH_INTERNAL void drain_thqueue(apth_thqueue_t queue, drain_thqueue_th_func fn);
APTH_INTERNAL void transfer_th(apth_t th, apth_thqueue_t from, apth_thqueue_t to);
APTH_INTERNAL apth_t transfer_one_th(apth_thqueue_t from, apth_thqueue_t to,
                                     bool insert_from_front, const char *dbg_msg);
APTH_INTERNAL size_t visit_thqueue(apth_thqueue_t queue, visit_thqueue_th_func fn, void *);
APTH_INTERNAL apth_t find_first_in_thqueue(apth_thqueue_t queue, find_first_in_thqueue_th_func fn, void *aux);

// ============================== Worker ==============================

APTH_INTERNAL void worker_key_t_init(void);
APTH_INTERNAL void worker_key_t_drop(void);
APTH_INTERNAL void sched_key_t_init(void);
APTH_INTERNAL void sched_key_t_drop(void);
APTH_INTERNAL apth_worker_t cur_worker(void);
APTH_INTERNAL void set_cur_worker(apth_worker_t worker);
APTH_INTERNAL apth_sched_t cur_sched(void);
APTH_INTERNAL void set_cur_sched(apth_sched_t sched);
APTH_INTERNAL apth_t cur_apth(void);
APTH_INTERNAL void set_cur_apth(apth_t t);
APTH_INTERNAL apth_worker_t get_worker_by_id(int worker_id);
APTH_INTERNAL int worker_count(void);
APTH_INTERNAL int apth_global_scheduler_pool_init(int init_workers);
APTH_INTERNAL int apth_global_scheduler_pool_drop(void);
APTH_INTERNAL int add_worker_thread(void);
APTH_INTERNAL bool is_main_worker(apth_worker_t worker);

// ============================== Scheduler ==============================

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

// ============================== TCB ==============================
APTH_INTERNAL apth_t apth_tcb_alloc(size_t stacksize, void *stackaddr, size_t guardsize);
APTH_INTERNAL void apth_tcb_free(apth_t t);
APTH_INTERNAL char *apth_tcb_get_usable_stack_start(apth_t t);

// ============================== Filedescriptor ==============================
APTH_INTERNAL void apth_fd_table_init(void);
APTH_INTERNAL int apth_fd_acquire(int fd);        // Increment refcount, set NONBLOCK when first acquired
APTH_INTERNAL void apth_fd_release(int fd);       // Decrement refcount, restore original flag when rc == 0
APTH_INTERNAL void apth_fd_register(int fd);      // Register when socket/open
APTH_INTERNAL void apth_fd_unregister(int fd);    // Unregister when close
APTH_INTERNAL void apth_notify_fd_closed(int fd); // Notify all schedulers about fd close

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
APTH_INTERNAL apth_cxt_t apth_ctx_alloc(void);
APTH_INTERNAL bool apth_ctx_save(apth_cxt_t ctx);
APTH_INTERNAL void apth_ctx_restore(apth_cxt_t ctx);
APTH_INTERNAL void apth_ctx_switch(apth_cxt_t old, apth_cxt_t new);
APTH_INTERNAL bool apth_ctx_set(apth_cxt_t ctx, void (*func)(void),
                                char *stack_mem_start, size_t stacksize);

// ============================== Cleanup ==============================
APTH_INTERNAL void apth_thread_cleanup(apth_t th);

// ============================== Cancel ==============================
APTH_INTERNAL void apth_do_cancel(void *result);
APTH_INTERNAL void apth_cancel_point(void);

// ============================== Signal ==============================
APTH_INTERNAL int apth_signal_system_init(void);
APTH_INTERNAL int apth_signal_system_drop(void);
APTH_INTERNAL void apth_deliver_pending_signals(apth_t th);

// APTH_INTERNAL int apth_util_sigdelete(int sig);
APTH_INTERNAL int apth_attr_setsigmask_internal(apth_attr_t *attr, const sigset_t *sigmask);
APTH_INTERNAL void apth_check_process_signals(apth_sched_t sched);
APTH_INTERNAL int apth_install_kernel_signal_catchers(void);

// ============================== TLS ==============================
APTH_INTERNAL void apth_key_destroydata(apth_t th);

// ============================== Event ==============================
APTH_INTERNAL void apth_sched_eventmanager(apth_sched_t sched, apth_time_t *now, bool dopoll);
APTH_INTERNAL void apth_sched_eventmanager_epoll(apth_sched_t sched, apth_time_t *now, bool dopoll);
APTH_INTERNAL void apth_sched_process_pending_fd_closes(apth_sched_t sched);
APTH_INTERNAL void apth_event_list_add(struct list *el, apth_event_t ev);
APTH_INTERNAL void apth_event_isolate(apth_event_t ev);
APTH_INTERNAL int apth_wait_event_list(struct list *el);
APTH_INTERNAL bool apth_wait_event(apth_event_t ev);
APTH_INTERNAL apth_event_t apth_event_fd(unsigned long spec, int fd);
APTH_INTERNAL apth_event_t apth_event_select(unsigned long spec, int *n, int nfd,
                                             fd_set *rfds, fd_set *wfds, fd_set *efds);
APTH_INTERNAL apth_event_t apth_event_sigs(unsigned long spec, const sigset_t *sigs, int *sig);
APTH_INTERNAL apth_event_t apth_event_time(unsigned long spec, apth_time_t tv);
APTH_INTERNAL apth_event_t apth_event_tid(unsigned long spec, apth_t tid);
APTH_INTERNAL apth_event_t apth_event_func(unsigned long spec, apth_event_custom_func_t func,
                                           void *arg, apth_time_t tv);
APTH_INTERNAL bool apth_event_free(apth_event_t ev);
APTH_INTERNAL bool apth_state_matches_event_goal(apth_state_t state, apth_goal_t goal);

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

// Returns 0 if ST is a valid stack size for a thread stack and EINVAL otherwise.
APTH_INTERNAL int check_stacksize_attr(size_t st);

int apth_apth_exists(apth_t t);

#endif /* __LIBAPTH_INTERNAL_FUNCS_H */
