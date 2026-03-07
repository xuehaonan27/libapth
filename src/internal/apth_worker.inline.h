#ifndef __LIBAPTH_INTERNAL_APTH_WORKER_INLINE_H
#define __LIBAPTH_INTERNAL_APTH_WORKER_INLINE_H

#include "internal_types.h"
#include "utils/archplattoold.h"

// TLS implementation selection:
// - If APTH_CUR_USING_KEYWORD is defined, use _Thread_local/__thread for faster access
// - Otherwise, use pthread_getspecific/pthread_setspecific for compatibility
#ifdef APTH_CUR_USING_KEYWORD
extern APTH_THREAD_LOCAL apth_worker_t __cur_worker_tls;
extern APTH_THREAD_LOCAL apth_sched_t __cur_sched_tls;
#else
// Use pthread TLS API
extern pthread_key_t __CUR_WORKER_KEY;
extern pthread_key_t __CUR_SCHED_KEY;
#endif

INLINE_ALWAYS apth_worker_t cur_worker(void)
{
#ifdef APTH_CUR_USING_KEYWORD
    return __cur_worker_tls;
#else
    return (apth_worker_t)apth_func_raw(pthread_getspecific)(__CUR_WORKER_KEY);
#endif
}
INLINE_ALWAYS void set_cur_worker(apth_worker_t worker)
{
#ifdef APTH_CUR_USING_KEYWORD
    __cur_worker_tls = worker;
#else
    int result = apth_func_raw(pthread_setspecific)(__CUR_WORKER_KEY, worker);
    assert_msg(result == 0, "fail pthread_setspecific result = %d (%s)", result, strerror(result));
#endif
}
INLINE_ALWAYS apth_sched_t cur_sched(void)
{
#ifdef APTH_CUR_USING_KEYWORD
    return __cur_sched_tls;
#else
    return (apth_sched_t)apth_func_raw(pthread_getspecific)(__CUR_SCHED_KEY);
#endif
}
INLINE_ALWAYS void set_cur_sched(apth_sched_t sched)
{
#ifdef APTH_CUR_USING_KEYWORD
    __cur_sched_tls = sched;
#else
    int result = apth_func_raw(pthread_setspecific)(__CUR_SCHED_KEY, sched);
    assert_msg(result == 0, "fail pthread_setspecific result = %d (%s)", result, strerror(result));
#endif
}
INLINE_ALWAYS apth_t cur_apth(void)
{
    return cur_sched()->cur;
}
INLINE_ALWAYS void set_cur_apth(apth_t t)
{
    assert(t != APTH_NULL);
    cur_sched()->cur = t;
}

#endif // __LIBAPTH_INTERNAL_APTH_WORKER_INLINE_H
