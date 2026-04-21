/*
 * apth_safepoint.c -- Safepoint cooperation hooks
 *
 * Provides global pause/resume for GC-style stop-the-world, plus
 * state-change and preemption callbacks for JVM integration.
 */
#include "apth.h"
#include "common.h"
#include "internal/types.h"
#include "internal/apth_global_sched_pool.h"
#include "internal/apth_sched.h"
#include "utils/atomic_wrapper.h"
#include <stdatomic.h>
#include <sched.h>

// ==================== Global Pause ====================

// When true, schedulers must not dispatch any thread.
_Atomic(bool) __apth_global_pause = false;

// Per-class pause: when true, threads of that class are not dispatched.
_Atomic(bool) __apth_class_paused[APTH_CLASS_COUNT] = {false};

APTH_API int apth_request_pause_all(void)
{
    atomic_store_release(&__apth_global_pause, true);

    // Wake all schedulers so they notice the flag promptly
    int nworkers = GLOBAL_POOL.init_worker_count;
    for (int i = 0; i < nworkers; i++)
    {
        apth_worker_t w = GLOBAL_POOL.worker_ptr_mem_start[i];
        if (w != NULL && w->sched != NULL)
            apth_sched_wake(w->sched);
    }

    // Spin-wait until no scheduler has a RUNNING thread
    for (;;)
    {
        bool all_idle = true;
        for (int i = 0; i < nworkers; i++)
        {
            apth_worker_t w = GLOBAL_POOL.worker_ptr_mem_start[i];
            if (w != NULL && w->sched != NULL && w->sched->cur != NULL)
            {
                all_idle = false;
                break;
            }
        }
        if (all_idle)
            break;
        sched_yield();
    }

    return 0;
}

APTH_API int apth_resume_all(void)
{
    atomic_store_release(&__apth_global_pause, false);

    // Wake all schedulers so they resume dispatching
    int nworkers = GLOBAL_POOL.init_worker_count;
    for (int i = 0; i < nworkers; i++)
    {
        apth_worker_t w = GLOBAL_POOL.worker_ptr_mem_start[i];
        if (w != NULL && w->sched != NULL)
            apth_sched_wake(w->sched);
    }

    return 0;
}

// ==================== Per-Class Pause ====================

APTH_API int apth_pause_class(int thread_class)
{
    if (thread_class < 0 || thread_class >= APTH_CLASS_COUNT)
        return EINVAL;
    atomic_store_release(&__apth_class_paused[thread_class], true);

    int nworkers = GLOBAL_POOL.init_worker_count;
    for (int i = 0; i < nworkers; i++)
    {
        apth_worker_t w = GLOBAL_POOL.worker_ptr_mem_start[i];
        if (w != NULL && w->sched != NULL)
            apth_sched_wake(w->sched);
    }
    return 0;
}

APTH_API int apth_resume_class(int thread_class)
{
    if (thread_class < 0 || thread_class >= APTH_CLASS_COUNT)
        return EINVAL;
    atomic_store_release(&__apth_class_paused[thread_class], false);

    int nworkers = GLOBAL_POOL.init_worker_count;
    for (int i = 0; i < nworkers; i++)
    {
        apth_worker_t w = GLOBAL_POOL.worker_ptr_mem_start[i];
        if (w != NULL && w->sched != NULL)
            apth_sched_wake(w->sched);
    }
    return 0;
}

// ==================== Preferred Class ====================

APTH_API int apth_set_preferred_class(int thread_class)
{
    if (thread_class < -1 || thread_class >= APTH_CLASS_COUNT)
        return EINVAL;

    int nworkers = GLOBAL_POOL.init_worker_count;
    for (int i = 0; i < nworkers; i++)
    {
        apth_worker_t w = GLOBAL_POOL.worker_ptr_mem_start[i];
        if (w != NULL && w->sched != NULL)
            w->sched->preferred_class = thread_class;
    }
    return 0;
}

// ==================== Callbacks ====================

static apth_state_callback_t __state_callback = NULL;
static void *__state_callback_arg = NULL;

static apth_preempt_hook_t __preempt_hook = NULL;
static void *__preempt_hook_arg = NULL;

APTH_API int apth_set_state_callback(apth_state_callback_t cb, void *arg)
{
    __state_callback = cb;
    __state_callback_arg = arg;
    return 0;
}

APTH_API int apth_set_preempt_hook(apth_preempt_hook_t hook, void *arg)
{
    __preempt_hook = hook;
    __preempt_hook_arg = arg;
    return 0;
}

// Called internally when a thread state changes.
// old_state and new_state are APTH_STATE_* values.
void __apth_fire_state_callback(apth_t th, int old_state, int new_state)
{
    apth_state_callback_t cb = __state_callback;
    if (cb != NULL)
        cb(th, old_state, new_state, __state_callback_arg);
}

// Called internally from apth_preempt_check() before yield.
void __apth_fire_preempt_hook(apth_t th)
{
    apth_preempt_hook_t hook = __preempt_hook;
    if (hook != NULL)
        hook(th, __preempt_hook_arg);
}

// ==================== Per-Thread Yield Hooks ====================

APTH_API int apth_set_yield_hooks(apth_t th,
                                  apth_yield_hook_t pre_yield,
                                  apth_yield_hook_t post_resume,
                                  void *arg)
{
    if (th == NULL || !APTH_IS_VALID(th))
        return EINVAL;
    th->yield_hook_arg = arg;
    __atomic_store_n(&th->post_resume_hook, post_resume, __ATOMIC_RELEASE);
    __atomic_store_n(&th->pre_yield_hook, pre_yield, __ATOMIC_RELEASE);
    return 0;
}

APTH_API int apth_set_yield_resume_callback(apth_t th,
                                            apth_yield_hook_t callback,
                                            void *arg)
{
    if (th == NULL || !APTH_IS_VALID(th))
        return EINVAL;
    th->yield_resume_arg = arg;
    __atomic_store_n(&th->yield_resume_callback, callback, __ATOMIC_RELEASE);
    return 0;
}
