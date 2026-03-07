#include "internal_funcs.h"
#include "utils/lll_new.inline.h"  // NEW: Use new LLL types
#include "internal_types.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"

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
    (void)attr;
    if (cond == NULL)
        return EINVAL;

    struct apth_cond_st *c = APTH_COND_CAST(cond);

    lll_apth_init(&c->guard);
    list_init(&c->waiters);
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
    apth_t self = cur_apth();

    // Prepare stack-allocated waiter and event
    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_COND;
    w.ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    w.ev.epoll_registered = false;

    // Enqueue as waiter BEFORE releasing the mutex.
    // This ensures a signal between unlock and sleep is not lost.
    lll_apth_lock(&c->guard);
    list_push_back(&c->waiters, &w.elem);
    lll_apth_unlock(&c->guard);

    // Add event to our event list
    apth_event_list_add(&self->event_list, &w.ev);

    // Release the associated mutex
    apth_mutex_unlock(mutex);

    // Block
    atomic_store(&self->state, APTH_STATE_WAITING);  // NEW: Simple state transition
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    // --- woken up ---
    apth_event_isolate(&w.ev);

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
    apth_t self = cur_apth();

    // Prepare COND event (sync waiter)
    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_COND;
    w.ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    w.ev.epoll_registered = false;

    // Prepare TIME event (stack-allocated)
    struct apth_event_st timer_ev;
    timer_ev.ev_status = APTH_EV_STATUS_PENDING;
    timer_ev.ev_type = APTH_EVENT_TYPE_TIME;
    timer_ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    timer_ev.epoll_registered = false;
    timer_ev.ev_args.TIME.tv.tv_sec = abstime->tv_sec;
    timer_ev.ev_args.TIME.tv.tv_usec = abstime->tv_nsec / 1000;

    // Enqueue waiter BEFORE releasing the mutex
    lll_apth_lock(&c->guard);
    list_push_back(&c->waiters, &w.elem);
    lll_apth_unlock(&c->guard);

    // Add BOTH events to event list
    apth_event_list_add(&self->event_list, &w.ev);
    apth_event_list_add(&self->event_list, &timer_ev);

    // Release the associated mutex
    apth_mutex_unlock(mutex);

    // Block
    atomic_store(&self->state, APTH_STATE_WAITING);  // NEW: Simple state transition
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    // --- woken up ---
    apth_event_isolate(&w.ev);
    apth_event_isolate(&timer_ev);

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
        apth_sched_t ws = sched_of(w->th);

        lll_apth_unlock(&c->guard);
        apth_sched_wake(ws);
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
    // Use a simple approach: wake each scheduler as we go.
    while (!list_empty(&c->waiters))
    {
        struct list_elem *e = list_pop_front(&c->waiters);
        struct apth_sync_waiter *w = apth_sync_waiter_entry(e);

        w->ev.ev_status = APTH_EV_STATUS_OCCURRED;
        apth_sched_wake(sched_of(w->th));
    }

    lll_apth_unlock(&c->guard);
    return 0;
}
