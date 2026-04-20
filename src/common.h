#ifndef __LIBAPTH_COMMON_H
#define __LIBAPTH_COMMON_H

#include "utils/archplattoold.h"
#include "internal/forward_declare.h"

#ifndef APTH_CUR_USING_KEYWORD
#include "hook_libc/hook_pthread.h"
#include <pthread.h>
#endif

// TLS implementation selection.
// If APTH_CUR_USING_KEYWORD is defined, use _Thread_local/__thread for faster
// access. Otherwise, use pthread_getspecific/pthread_setspecific for compatibility
#ifdef APTH_CUR_USING_KEYWORD
extern APTH_THREAD_LOCAL apth_sched_t __cur_sched_tls;
#else
// Use pthread TLS API
extern pthread_key_t __CUR_SCHED_KEY;
#endif

#ifdef APTH_CUR_USING_KEYWORD
#define CUR_SCHED (__cur_sched_tls)
#define SET_CUR_SCHED(SCHED)       \
    do                             \
    {                              \
        __cur_sched_tls = (SCHED); \
    } while (0)
#else
#define CUR_SCHED ((apth_sched_t)apth_func_raw(pthread_getspecific)(__CUR_SCHED_KEY))
#define SET_CUR_SCHED(SCHED)                                                                            \
    do                                                                                                  \
    {                                                                                                   \
        int result = apth_func_raw(pthread_setspecific)(__CUR_SCHED_KEY, (SCHED));                      \
        assert_msg(result == 0, "fail pthread_setspecific result = %d (%s)", result, strerror(result)); \
    } while (0)
#endif

#define SCHED_OF(T) ((T)->current_sched)
#define QUEUE_STATE_OF(T) ((T)->current_queue ? (T)->current_queue->th_state : APTH_STATE_NEW)

// CUR_APTH returns the currently running apth_t, or NULL when the scheduler
// itself is running (no user thread dispatched), or when CUR_SCHED is NULL
// (post-shutdown, or a signal handler fires on a worker after its scheduler
// has been torn down).  Code that needs to distinguish scheduler context
// from thread context should simply compare against NULL.
#define CUR_APTH (CUR_SCHED ? CUR_SCHED->cur : (apth_t)NULL)

#define SET_CUR_APTH(T)       \
    do                        \
    {                         \
        CUR_SCHED->cur = (T); \
    } while (0)

#define APTH_TCB_NAMELEN 31

// Minimal stack size by bytes
#define APTH_MIN_STACK 16384

// Default stack size by bytes
#define APTH_STACK_SIZE_DEFAULT 2097152 // 2 MiB

// FIXME: this should be set by build system
// Indicate stack growth direction is from high to low (< 0) or from low to
// high (> 0). On most systems this should usually be the former.
#ifndef APTH_STACKGROWTH
#define APTH_STACKGROWTH (-1)
#endif

#define _BIT(n) (1 << (n))

extern _Atomic(unsigned int) WORKER_SPAWNED;
extern _Atomic(unsigned int) SYNC_BEFORE_MAIN_APTH_SPAWN;

extern _Atomic(int) MAIN_APTH_EXITED;
extern _Atomic(int) MAIN_APTH_EXITED_BY_CALLING_APTH_EXIT;

#include <paths.h>
#define APTH_PATH_BINSH _PATH_BSHELL

#endif /* __LIBAPTH_COMMON_H */
