#ifndef __LIBAPTH_HOOK_LIBC_HOOK_PTHREAD_H
#define __LIBAPTH_HOOK_LIBC_HOOK_PTHREAD_H

#include "hook_utils.h"
#include <pthread.h>
#include <sched.h> // For cpu_set_t
#include <bits/types/sigset_t.h>
#include <bits/types/clockid_t.h>

#define APTH_LIST_OF_HOOK_PTHREAD  \
    X(pthread_create)              \
    X(pthread_sigmask)             \
    X(pthread_self)                \
    X(pthread_kill)                \
    X(pthread_key_create)          \
    X(pthread_key_delete)          \
    X(pthread_getspecific)         \
    X(pthread_setspecific)         \
    X(pthread_attr_init)           \
    X(pthread_join)                \
    X(pthread_exit)                \
    X(pthread_attr_setdetachstate) \
    X(pthread_attr_setaffinity_np) \
    X(pthread_cancel)              \
    X(pthread_getcpuclockid)

// ==================== For these functions, only fetch its LIBC impls ====================
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_create, pthread_t *restrict thread,
                            const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_sigmask, int how, const sigset_t *set, sigset_t *oset)
APTH_DECLARE_FETCH_LIBCFUNC(pthread_t, pthread_self, void)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_kill, pthread_t thread, int sig)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_key_create, pthread_key_t *__key,
                            void (*__destr_function)(void *))
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_key_delete, pthread_key_t __key)
APTH_DECLARE_FETCH_LIBCFUNC(void *, pthread_getspecific, pthread_key_t __key)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_setspecific, pthread_key_t __key,
                            const void *__pointer)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_attr_init, pthread_attr_t *attr)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_join, pthread_t thread, void **retval)
APTH_DECLARE_FETCH_LIBCFUNC(void, pthread_exit, void *retval)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_attr_setdetachstate, pthread_attr_t *attr, int detachstate)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_attr_setaffinity_np, pthread_attr_t *attr,
                            size_t cpusetsize, const cpu_set_t *cpuset)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_cancel, pthread_t thread)
APTH_DECLARE_FETCH_LIBCFUNC(int, pthread_getcpuclockid, pthread_t thread, clockid_t *clockid);

#endif // __LIBAPTH_HOOK_LIBC_HOOK_PTHREAD_H
