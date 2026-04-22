#include "apth_cond.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_event.h"
#include "internal/apth_sched.h"
#include "internal/apth_sync_waiter.h"
#include "internal/apth_dedicated.h"
#include "utils/apth_errno.h"
#include "utils/lll.inline.h"
#include <time.h>
#include <sched.h>
#include <unistd.h>

// ==================== Condition Variable Attributes ====================

int apth_condattr_init(apth_condattr_t *attr)
{
    if (attr == NULL)
        return EINVAL;
    struct apth_condattr_st *a = APTH_CONDATTR_CAST(attr);
    a->pshared = 0;
    a->clock_id = CLOCK_REALTIME; // Default clock
    return 0;
}

int apth_condattr_destroy(apth_condattr_t *attr)
{
    if (attr == NULL)
        return EINVAL;
    // No-op for stack-allocated opaque union
    return 0;
}

int apth_condattr_setclock(apth_condattr_t *attr, clockid_t clock_id)
{
    if (attr == NULL)
        return EINVAL;

    // Validate clock_id - only support CLOCK_REALTIME and CLOCK_MONOTONIC
    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC)
        return EINVAL;

    struct apth_condattr_st *a = APTH_CONDATTR_CAST(attr);
    a->clock_id = clock_id;
    return 0;
}

int apth_condattr_getclock(const apth_condattr_t *attr, clockid_t *clock_id)
{
    if (attr == NULL || clock_id == NULL)
        return EINVAL;

    const struct apth_condattr_st *a = APTH_CONDATTR_CONST_CAST(attr);
    *clock_id = a->clock_id;
    return 0;
}

// ==================== Condition Variable ====================

int apth_cond_init(apth_cond_t *cond, const apth_condattr_t *attr)
{
    if (cond == NULL)
        return EINVAL;

    struct apth_cond_st *c = APTH_COND_CAST(cond);

    lll_apth_init(&c->guard);
    list_init(&c->waiters);

    // Store clock_id from attributes (default CLOCK_REALTIME)
    if (attr != NULL)
    {
        const struct apth_condattr_st *a = APTH_CONDATTR_CONST_CAST(attr);
        c->clock_id = a->clock_id;
    }
    else
    {
        c->clock_id = CLOCK_REALTIME;
    }

    return 0;
}

int apth_cond_destroy(apth_cond_t *cond)
{
    if (cond == NULL)
        return EINVAL;

    struct apth_cond_st *c = APTH_COND_CAST(cond);

    lll_apth_lock(&c->guard);
    if (!list_empty(&c->waiters))
    {
        lll_apth_unlock(&c->guard);
        return EBUSY;
    }
    lll_apth_unlock(&c->guard);

    // No free() needed - cond is stack-allocated
    return 0;
}

int apth_cond_wait(apth_cond_t *cond, apth_mutex_t *mutex)
{
    if (cond == NULL || mutex == NULL)
        return EINVAL;

    struct apth_cond_st *c = APTH_COND_CAST(cond);
    apth_t self = CUR_APTH;

    // No apth context: cannot enter the wait queue.  Release the mutex,
    // yield to OS, re-acquire — POSIX allows spurious wakeups.
    if (self == NULL)
    {
        apth_mutex_unlock(mutex);
        sched_yield();
        apth_mutex_lock(mutex);
        return 0;
    }

    // Prepare stack-allocated waiter and event
    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_SYNC;
    w.ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    w.ev.epoll_registered = false;

    // Enqueue as waiter BEFORE releasing the mutex.
    // This ensures a signal between unlock and sleep is not lost.
    lll_apth_lock(&c->guard);
    list_push_back(&c->waiters, &w.elem);
    lll_apth_unlock(&c->guard);

    // Add event to our event list.
    // Dedicated threads have no scheduler event manager, so skip this.
    if (!self->is_dedicated)
        apth_event_list_add(&self->event_list, &w.ev);

    // Release the associated mutex
    apth_mutex_unlock(mutex);

    if (self->is_dedicated)
    {
        // Dedicated threads block on their wake futex instead of yielding
        int __cw_iters = 0;
        while (atomic_load_acquire(&w.ev.ev_status) != APTH_EV_STATUS_OCCURRED)
        {
            apth_dedicated_block(self);
            if (++__cw_iters >= 4 && __cw_iters % 4 == 0)
            {
                char __buf[256];
                int __n = snprintf(__buf, sizeof(__buf),
                    "[LIBAPTH] cond_wait stuck ~%ds: t=%p name=%s cond=%p ev_status=%d\n",
                    __cw_iters * 3, (void *)self,
                    self->name ? self->name : "?",
                    (void *)c, (int)w.ev.ev_status);
                if (__n > 0) write(STDERR_FILENO, __buf, __n);
            }
        }
    }
    else
    {
        // Block
        atomic_store_release(&self->state, APTH_STATE_WAITING);
        self->yield_reason = APTH_YIELD_REASON_WAIT;
        apth_yield();

        // --- woken up ---
        apth_event_isolate(&w.ev);
    }

    // Re-acquire the mutex
    apth_mutex_lock(mutex);

    return 0;
}

int apth_cond_timedwait(apth_cond_t *cond, apth_mutex_t *mutex,
                        const struct timespec *abstime)
{
    if (cond == NULL || mutex == NULL)
        return EINVAL;
    if (abstime == NULL)
        return EINVAL;

    struct apth_cond_st *c = APTH_COND_CAST(cond);
    apth_t self = CUR_APTH;

    if (self == NULL)
    {
        apth_mutex_unlock(mutex);
        struct timespec now;
        clock_gettime(c->clock_id, &now);
        if (now.tv_sec > abstime->tv_sec ||
            (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec))
        {
            apth_mutex_lock(mutex);
            return ETIMEDOUT;
        }
        sched_yield();
        apth_mutex_lock(mutex);
        return 0;
    }

    // Prepare COND event (sync waiter)
    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_SYNC;
    w.ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    w.ev.epoll_registered = false;

    // Prepare TIME event (stack-allocated).
    // The internal timer uses gettimeofday (CLOCK_REALTIME).
    // If the cond was configured with CLOCK_MONOTONIC, convert the
    // absolute deadline to a CLOCK_REALTIME-based absolute time.
    struct apth_event_st timer_ev;
    timer_ev.ev_status = APTH_EV_STATUS_PENDING;
    timer_ev.ev_type = APTH_EVENT_TYPE_TIME;
    timer_ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    timer_ev.epoll_registered = false;

    if (c->clock_id == CLOCK_MONOTONIC)
    {
        // Convert CLOCK_MONOTONIC abstime to CLOCK_REALTIME:
        // realtime_deadline = now_realtime + (abstime - now_monotonic)
        struct timespec mono_now, real_now;
        clock_gettime(CLOCK_MONOTONIC, &mono_now);
        clock_gettime(CLOCK_REALTIME, &real_now);

        long long diff_sec = abstime->tv_sec - mono_now.tv_sec;
        long diff_nsec = abstime->tv_nsec - mono_now.tv_nsec;
        if (diff_nsec < 0) { diff_sec--; diff_nsec += 1000000000L; }

        long long real_sec = real_now.tv_sec + diff_sec;
        long real_nsec = real_now.tv_nsec + diff_nsec;
        if (real_nsec >= 1000000000L) { real_sec++; real_nsec -= 1000000000L; }

        timer_ev.ev_args.TIME.tv.tv_sec = (long)real_sec;
        timer_ev.ev_args.TIME.tv.tv_usec = real_nsec / 1000;
    }
    else
    {
        // CLOCK_REALTIME: abstime is already in the right clock domain
        timer_ev.ev_args.TIME.tv.tv_sec = abstime->tv_sec;
        timer_ev.ev_args.TIME.tv.tv_usec = abstime->tv_nsec / 1000;
    }

    // Enqueue waiter BEFORE releasing the mutex
    lll_apth_lock(&c->guard);
    list_push_back(&c->waiters, &w.elem);
    lll_apth_unlock(&c->guard);

    // Add events to event list.
    // Dedicated threads have no scheduler event manager, so skip this.
    if (!self->is_dedicated)
    {
        apth_event_list_add(&self->event_list, &w.ev);
        apth_event_list_add(&self->event_list, &timer_ev);
    }

    // Release the associated mutex
    apth_mutex_unlock(mutex);

    if (self->is_dedicated)
    {
        // Dedicated threads: poll with timeout via eventfd + clock check.
        // Compute the real-time deadline for the clock check.
        // (timer_ev already has the converted real-time deadline)
        struct timespec now;
        while (atomic_load_acquire(&w.ev.ev_status) != APTH_EV_STATUS_OCCURRED)
        {
            clock_gettime(CLOCK_REALTIME, &now);
            long deadline_sec = timer_ev.ev_args.TIME.tv.tv_sec;
            long deadline_usec = timer_ev.ev_args.TIME.tv.tv_usec;
            if (now.tv_sec > deadline_sec ||
                (now.tv_sec == deadline_sec &&
                 now.tv_nsec / 1000 >= deadline_usec))
                break; // Timed out
            sched_yield(); // Brief yield to OS, then re-check
        }
    }
    else
    {
        // Block
        atomic_store_release(&self->state, APTH_STATE_WAITING);
        self->yield_reason = APTH_YIELD_REASON_WAIT;
        apth_yield();

        // --- woken up ---
        apth_event_isolate(&w.ev);
        apth_event_isolate(&timer_ev);
    }

    // Resolve race: signal/broadcast vs timeout
    int ret = 0;
    lll_apth_lock(&c->guard);

    if (w.ev.ev_status != APTH_EV_STATUS_OCCURRED)
    {
        // Timer fired first; we're still in the waiters list
        list_remove(&w.elem);
        ret = ETIMEDOUT;
    }

    lll_apth_unlock(&c->guard);

    // Re-acquire the mutex (required by POSIX, even on timeout)
    apth_mutex_lock(mutex);

    return ret;
}

int apth_cond_signal(apth_cond_t *cond)
{
    if (cond == NULL)
        return EINVAL;

    struct apth_cond_st *c = APTH_COND_CAST(cond);

    lll_apth_lock(&c->guard);

    if (!list_empty(&c->waiters))
    {
        struct list_elem *e = list_pop_front(&c->waiters);
        struct apth_sync_waiter *w = apth_sync_waiter_entry(e);

        // Direct wakeup
        w->ev.ev_status = APTH_EV_STATUS_OCCURRED;

        if (w->th->is_dedicated)
        {
            lll_apth_unlock(&c->guard);
            apth_dedicated_unblock(w->th);
        }
        else
        {
            apth_sched_t ws = SCHED_OF(w->th);
            lll_apth_unlock(&c->guard);
            apth_sched_wake(ws);
        }
        return 0;
    }

    lll_apth_unlock(&c->guard);
    return 0;
}

int apth_cond_broadcast(apth_cond_t *cond)
{
    if (cond == NULL)
        return EINVAL;

    struct apth_cond_st *c = APTH_COND_CAST(cond);

    lll_apth_lock(&c->guard);

    // Collect all schedulers that need waking.
    // Use a simple approach: wake each scheduler/thread as we go.
    while (!list_empty(&c->waiters))
    {
        struct list_elem *e = list_pop_front(&c->waiters);
        struct apth_sync_waiter *w = apth_sync_waiter_entry(e);

        w->ev.ev_status = APTH_EV_STATUS_OCCURRED;

        if (w->th->is_dedicated)
            apth_dedicated_unblock(w->th);
        else
            apth_sched_wake(SCHED_OF(w->th));
    }

    lll_apth_unlock(&c->guard);
    return 0;
}
