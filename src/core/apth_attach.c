#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "apth.h"
#include "common.h"
#include "internal/types.h"
#include "internal/apth_dedicated.h"
#include "internal/apth_tcb.h"
#include "internal/apth_time.h"
#include "internal/apth_sched.h"
#include "hook_libc/hooked_funcs.h"
#include "utils/apth_errno.h"
#include "utils/lll.inline.h"
#include "utils/list.inline.h"
#include "utils/atomic_wrapper.h"
#include "utils/debug.h"

// External references
extern _Atomic(unsigned int) apth_nthreads;
extern lll_internal_t __dedicated_registry_lock;
extern struct list __dedicated_registry;

/**
 * Attach the calling pthread as a DEDICATED apth.
 *
 * After this call, CUR_APTH is valid and all LIBAPTH APIs
 * (apth_getspecific, apth_mutex_lock, etc.) work on this thread.
 * The thread bypasses the userspace scheduler entirely.
 */
int apth_attach_self_as_dedicated(apth_t *out)
{
    if (out == NULL)
        return apth_error(EINVAL, EINVAL);

    if (CUR_SCHED != NULL && CUR_APTH != NULL)
        return apth_error(EEXIST, EEXIST);

    // Allocate TCB
    apth_t t = (apth_t)malloc(sizeof(struct apth_st));
    if (t == NULL)
        return apth_error(ENOMEM, ENOMEM);
    memset(t, '\0', sizeof(struct apth_st));

    // Magic and class
    t->magic = APTH_MAGIC;
    t->thread_class = APTH_CLASS_DEDICATED;
    atomic_store_release(&t->state, APTH_STATE_RUNNING);

    // Stack fields — we don't own the stack
    t->stack_mem_start = NULL;
    t->stackloan = true;
    t->stacksize = 0;
    t->guardsize = 0;
    list_init(&t->event_list);

    // Name
    t->name[0] = '\0';

    // Time fields
    apth_time_t ts;
    apth_time_set(&ts, APTH_TIME_NOW);
    apth_time_set(&t->spawned, &ts);
    apth_time_set(&t->lastran, &ts);
    apth_time_set(&t->running, APTH_TIME_ZERO);

    // Signals
    t->wake_pending = false;
    sigemptyset(&t->sigpending);
    t->sigpendcnt = 0;
    lll_internal_init(&t->siglock);
    t->in_sighandler = false;
    t->sigaltstack_set = false;

    // Get current signal mask from the real pthread
    pthread_sigmask(SIG_SETMASK, NULL, &t->sigmask);

    // Ownership
    t->current_queue = NULL;
    lll_internal_init(&t->ownership_lock);

    // Start routine — not applicable for attached threads
    t->start_func = NULL;
    t->start_arg = NULL;

    // Join — self (not joinable by default for attached threads)
    t->join_arg = NULL;
    t->joinid = t;

    // Cancellation
    atomic_store_release(&t->cancelreq, false);
    t->cancelhandling = 0;
    t->cleanups = NULL;

    // TLS
    t->specific[0] = t->specific_1stblock;
    t->specific_used = false;

    // Yield
    t->last_yield_tick = cpu_tick();
    t->yield_timeslice = 10;
    t->yield_reason = APTH_YIELD_REASON_VOLUNTEER;

    // Dedicated thread fields
    t->is_dedicated = true;
    t->dispatch_prevented = false;
    t->dedicated_tid = apth_func_raw(pthread_self)();
    t->dedicated_wake_fd = eventfd(0, EFD_CLOEXEC);
    if (t->dedicated_wake_fd < 0)
    {
        free(t);
        return apth_error(errno, errno);
    }

    // Allocate dummy scheduler for TLS compatibility
    t->dedicated_dummy_sched = (apth_sched_t)calloc(1, sizeof(struct apth_sched_st));
    if (t->dedicated_dummy_sched == NULL)
    {
        apth_func_raw(close)(t->dedicated_wake_fd);
        free(t);
        return apth_error(ENOMEM, ENOMEM);
    }
    t->dedicated_dummy_sched->id = -1;
    t->dedicated_dummy_sched->wake_eventfd = -1;
    t->dedicated_dummy_sched->epoll_fd = -1;
    t->current_sched = t->dedicated_dummy_sched;

    // Set TLS so CUR_SCHED and CUR_APTH macros work
    SET_CUR_SCHED(t->dedicated_dummy_sched);
    t->dedicated_dummy_sched->cur = t;
    // Note: SET_CUR_APTH(t) would expand to CUR_SCHED->cur = t,
    // which is the same as the line above.

    // Block SIGPROF — preemption timer must not fire on dedicated threads
    sigset_t ss_block;
    sigemptyset(&ss_block);
    sigaddset(&ss_block, SIGPROF);
    pthread_sigmask(SIG_BLOCK, &ss_block, NULL);

    // Register in dedicated registry
    lll_internal_lock(&__dedicated_registry_lock);
    list_push_back(&__dedicated_registry, &t->dedicated_elem);
    lll_internal_unlock(&__dedicated_registry_lock);

    // Increment global counters
    atomic_fetch_add_release(&apth_nthreads, 1);
    inc_alive_thrcnt();

    apth_debug("attached pthread as dedicated apth %p", t);
    *out = t;
    return 0;
}

/**
 * Detach the calling thread from LIBAPTH.
 *
 * Reverses apth_attach_self_as_dedicated(): removes the TCB,
 * clears TLS, and frees all resources.
 */
int apth_detach_self(void)
{
    if (CUR_SCHED == NULL)
        return apth_error(EINVAL, EINVAL);

    apth_t t = CUR_APTH;
    if (t == NULL || !t->is_dedicated)
        return apth_error(EINVAL, EINVAL);

    // Remove from dedicated registry
    lll_internal_lock(&__dedicated_registry_lock);
    list_remove(&t->dedicated_elem);
    lll_internal_unlock(&__dedicated_registry_lock);

    // Decrement global counters
    atomic_decrement_if_positive(&apth_nthreads);
    dec_alive_thrcnt();

    // Clear TLS
    SET_CUR_APTH(NULL);
    SET_CUR_SCHED(NULL);

    // Free resources
    if (t->dedicated_wake_fd >= 0)
        apth_func_raw(close)(t->dedicated_wake_fd);
    free(t->dedicated_dummy_sched);

    t->magic = 0; // Invalidate
    free(t);

    apth_debug("detached dedicated apth");
    return 0;
}

/**
 * Prevent the scheduler from dispatching this thread.
 * Used for JVM safepoints.
 */
int apth_prevent_dispatch(apth_t t)
{
    if (!APTH_IS_VALID(t))
        return apth_error(EINVAL, EINVAL);

    atomic_store_release(&t->dispatch_prevented, true);
    return 0;
}

/**
 * Allow the scheduler to dispatch this thread again.
 */
int apth_allow_dispatch(apth_t t)
{
    if (!APTH_IS_VALID(t))
        return apth_error(EINVAL, EINVAL);

    atomic_store_release(&t->dispatch_prevented, false);
    return 0;
}
