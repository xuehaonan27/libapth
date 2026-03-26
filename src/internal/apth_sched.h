#ifndef __LIBAPTH_INTERNAL_APTH_SCHED_H
#define __LIBAPTH_INTERNAL_APTH_SCHED_H

#include "internal/types/struct_apth_sched_st.h"

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

// Global thread counters (needed by dedicated thread creation/destruction)
extern _Atomic(unsigned int) apth_nthreads;
APTH_INTERNAL void inc_alive_thrcnt(void);
APTH_INTERNAL void dec_alive_thrcnt(void);
APTH_INTERNAL unsigned int get_apth_alive_nthreads(void);

APTH_INTERNAL bool apth_sched_is_opening(apth_sched_t sched);
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
