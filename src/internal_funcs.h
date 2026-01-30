#ifndef __LIBAPTH_INTERNAL_FUNCS_H
#define __LIBAPTH_INTERNAL_FUNCS_H

#include "internal_types.h"

// ============================== Worker ==============================

void worker_key_t_init(void);
// apth_worker_t cur_worker(void);
// void set_cur_worker(apth_worker_t worker);
// apth_sched_t cur_sched(void);
apth_t cur_apth(void);
void set_cur_apth(apth_t t);
void worker_key_t_destr_fn(void *p);
apth_worker_t get_worker_by_id(int worker_id);
int worker_count(void);
int apth_global_scheduler_pool_init(bool caller_pthr_gets_involved);
int add_worker_thread(void);

// ============================== Scheduler ==============================

void apth_scheduler_init(apth_sched_t sched, apth_worker_t worker);
void inc_thrcnt(apth_sched_t sched);
void dec_thrcnt(apth_sched_t sched);

#define push_apth_to(name) push_apth_to_##name
#define pop_apth_from(name) pop_apth_from_##name
#define DECLARE_SCHED_LIST_OP(name)                         \
    void push_apth_to(name)(apth_t th, apth_sched_t sched); \
    apth_t pop_apth_from(name)(apth_sched_t sched);
DECLARE_SCHED_LIST_OP(new)
DECLARE_SCHED_LIST_OP(ready)
DECLARE_SCHED_LIST_OP(waiting)
DECLARE_SCHED_LIST_OP(suspended)
DECLARE_SCHED_LIST_OP(terminated)
#undef DECLARE_SCHED_LIST_OP
#undef pop_apth_from
#undef push_apth_to

bool apth_sched_is_opening(apth_sched_t sched);

// ============================== TCB ==============================
apth_t apth_tcb_alloc(size_t stacksize, void *stackaddr);
void apth_tch_free(apth_t t);

// ============================== Time ==============================
uint64_t cpu_tick();
apth_time_t apth_time(long sec, long usec);
void apth_time_set(apth_time_t *t1, apth_time_t *t2);
void apth_time_add(apth_time_t *t1, apth_time_t *t2);
void apth_time_sub(apth_time_t *t1, apth_time_t *t2);

// ============================== System call ==============================

// ============================== Context ==============================
bool apth_ctx_save(apth_cxt_t ctx);
void apth_ctx_restore(apth_cxt_t ctx);
void apth_ctx_switch(apth_cxt_t old, apth_cxt_t new);
bool apth_ctx_set(apth_cxt_t ctx, void (*func)(void), char *stack_addr_lo, char *stack_addr_hi);

apth_t apth_tcb_alloc(size_t, void *);
void apth_tch_free(apth_t);

bool apth_cleanup_push(void (*)(void *), void *);
bool apth_cleanup_pop(bool);
void apth_cleanup_popall(apth_t, bool);

#endif /* __LIBAPTH_INTERNAL_FUNCS_H */
