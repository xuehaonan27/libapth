#include "apth_mutex.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_sync_waiter.h"
#include "internal/apth_sched.h"
#include "internal/apth_event.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include "utils/lll_new.inline.h"

// ==================== Mutex Attributes ====================

int apth_mutexattr_init(apth_mutexattr_t *attr)
{
    if (attr == NULL)
        return EINVAL;
    struct apth_mutexattr_st *a = APTH_MUTEXATTR_CAST(attr);
    a->type = APTH_MUTEX_DEFAULT;
    return 0;
}

int apth_mutexattr_destroy(apth_mutexattr_t *attr)
{
    if (attr == NULL)
        return EINVAL;
    // No-op for stack-allocated opaque union
    return 0;
}

int apth_mutexattr_gettype(const apth_mutexattr_t *attr, int *type)
{
    if (attr == NULL || type == NULL)
        return EINVAL;
    const struct apth_mutexattr_st *a = APTH_MUTEXATTR_CONST_CAST(attr);
    *type = a->type;
    return 0;
}

int apth_mutexattr_settype(apth_mutexattr_t *attr, int type)
{
    if (attr == NULL)
        return EINVAL;
    if (type != APTH_MUTEX_NORMAL &&
        type != APTH_MUTEX_ERRORCHECK &&
        type != APTH_MUTEX_RECURSIVE)
        return EINVAL;
    struct apth_mutexattr_st *a = APTH_MUTEXATTR_CAST(attr);
    a->type = type;
    return 0;
}

// ==================== Mutex ====================

int apth_mutex_init(apth_mutex_t *mutex, const apth_mutexattr_t *attr)
{
    if (mutex == NULL)
        return EINVAL;

    struct apth_mutex_st *m = APTH_MUTEX_CAST(mutex);

    lll_apth_init(&m->guard);  // NEW: Use Type 1 LLL init
    m->owner = NULL;
    m->lock_count = 0;
    m->type = (attr != NULL) ? APTH_MUTEXATTR_CONST_CAST(attr)->type : APTH_MUTEX_DEFAULT;
    list_init(&m->waiters);
    return 0;
}

int apth_mutex_destroy(apth_mutex_t *mutex)
{
    if (mutex == NULL)
        return EINVAL;

    struct apth_mutex_st *m = APTH_MUTEX_CAST(mutex);

    lll_apth_lock(&m->guard);  // NEW: Use Type 1 LLL
    if (!list_empty(&m->waiters))
    {
        lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL
        return EBUSY;
    }
    if (m->owner != NULL)
    {
        lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL
        return EBUSY;
    }
    lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL

    // No free() needed - mutex is stack-allocated
    return 0;
}

int apth_mutex_lock(apth_mutex_t *mutex)
{
    if (mutex == NULL)
        return EINVAL;

    struct apth_mutex_st *m = APTH_MUTEX_CAST(mutex);
    apth_t self = CUR_APTH;

    lll_apth_lock(&m->guard);  // NEW: Use Type 1 LLL

    // Fast path: mutex is free
    if (m->owner == NULL)
    {
        m->owner = self;
        m->lock_count = 1;
        lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL
        return 0;
    }

    // Same-owner cases
    if (m->owner == self)
    {
        if (m->type == APTH_MUTEX_RECURSIVE)
        {
            m->lock_count++;
            lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL
            return 0;
        }
        if (m->type == APTH_MUTEX_ERRORCHECK)
        {
            lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL
            return EDEADLK;
        }
        // NORMAL: undefined behavior per POSIX; we block (will deadlock).
    }

    // Slow path: must block.
    // Prepare stack-allocated waiter and event.
    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_MUTEX;
    w.ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    w.ev.epoll_registered = false;

    // Enqueue waiter (FIFO order)
    list_push_back(&m->waiters, &w.elem);

    // Add event to our thread's event list (so event manager can see it)
    apth_event_list_add(&self->event_list, &w.ev);

    // Release guard BEFORE yielding to avoid scheduler-level deadlock.
    // Between this unlock and the yield, another thread could call unlock()
    // and mark our event as OCCURRED — that's fine; the event manager will
    // immediately re-ready us.
    lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL

    atomic_store_release(&self->state, APTH_STATE_WAITING);  // NEW: Simple state transition
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    // --- woken up ---
    // unlock() already set m->owner = self and removed us from waiters.
    apth_event_isolate(&w.ev);

    return 0;
}

int apth_mutex_timedlock(apth_mutex_t *mutex, const struct timespec *abstime)
{
    if (mutex == NULL)
        return EINVAL;
    if (abstime == NULL)
        return EINVAL;

    struct apth_mutex_st *m = APTH_MUTEX_CAST(mutex);
    apth_t self = CUR_APTH;

    lll_apth_lock(&m->guard);  // NEW: Use Type 1 LLL

    // Fast path: mutex is free
    if (m->owner == NULL)
    {
        m->owner = self;
        m->lock_count = 1;
        lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL
        return 0;
    }

    // Same-owner cases
    if (m->owner == self)
    {
        if (m->type == APTH_MUTEX_RECURSIVE)
        {
            m->lock_count++;
            lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL
            return 0;
        }
        if (m->type == APTH_MUTEX_ERRORCHECK)
        {
            lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL
            return EDEADLK;
        }
    }

    // Slow path: must block with timeout.
    // Prepare MUTEX event (sync waiter)
    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_MUTEX;
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

    // Enqueue waiter
    list_push_back(&m->waiters, &w.elem);

    // Add BOTH events to the thread's event list
    apth_event_list_add(&self->event_list, &w.ev);
    apth_event_list_add(&self->event_list, &timer_ev);

    lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL

    atomic_store_release(&self->state, APTH_STATE_WAITING);  // NEW: Simple state transition
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    // --- woken up ---
    // Isolate BOTH events from the event list
    apth_event_isolate(&w.ev);
    apth_event_isolate(&timer_ev);

    // Resolve race: did unlock() transfer ownership, or did the timer fire?
    lll_apth_lock(&m->guard);  // NEW: Use Type 1 LLL

    if (w.ev.ev_status == APTH_EV_STATUS_OCCURRED)
    {
        // unlock() already dequeued us and transferred ownership
        lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL
        return 0;
    }

    // Timer fired first; we're still in the waiters list. Remove ourselves.
    list_remove(&w.elem);
    lll_apth_unlock(&m->guard);
    return ETIMEDOUT;
}

int apth_mutex_trylock(apth_mutex_t *mutex)
{
    if (mutex == NULL)
        return EINVAL;

    struct apth_mutex_st *m = APTH_MUTEX_CAST(mutex);
    apth_t self = CUR_APTH;

    // Use trylock for non-blocking guard acquisition
    if (lll_apth_trylock(&m->guard) != 0)
        return EBUSY;

    if (m->owner == NULL)
    {
        m->owner = self;
        m->lock_count = 1;
        lll_apth_unlock(&m->guard);
        return 0;
    }

    if (m->owner == self && m->type == APTH_MUTEX_RECURSIVE)
    {
        m->lock_count++;
        lll_apth_unlock(&m->guard);
        return 0;
    }

    lll_apth_unlock(&m->guard);
    return EBUSY;
}

int apth_mutex_unlock(apth_mutex_t *mutex)
{
    if (mutex == NULL)
        return EINVAL;

    struct apth_mutex_st *m = APTH_MUTEX_CAST(mutex);
    apth_t self = CUR_APTH;

    lll_apth_lock(&m->guard);  // NEW: Use Type 1 LLL

    // Error check
    if (m->type == APTH_MUTEX_ERRORCHECK && m->owner != self)
    {
        lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL
        return EPERM;
    }

    // Recursive decrement
    if (m->type == APTH_MUTEX_RECURSIVE && m->lock_count > 1)
    {
        m->lock_count--;
        lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL
        return 0;
    }

    // Check waiter queue
    if (list_empty(&m->waiters))
    {
        // No waiters — simply release
        m->owner = NULL;
        m->lock_count = 0;
        lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL
        return 0;
    }

    // Pop first waiter and transfer ownership
    struct list_elem *e = list_pop_front(&m->waiters);
    struct apth_sync_waiter *w = apth_sync_waiter_entry(e);

    m->owner = w->th;
    m->lock_count = 1;

    // Direct wakeup: mark the waiter's event as OCCURRED
    w->ev.ev_status = APTH_EV_STATUS_OCCURRED;

    // Save scheduler pointer before releasing guard (waiter struct is
    // on the waiter's stack, still valid while it's in WAITING state)
    apth_sched_t waiter_sched = SCHED_OF(w->th);

    lll_apth_unlock(&m->guard);  // NEW: Use Type 1 LLL

    // Prod the waiter's scheduler so it notices the OCCURRED event
    apth_sched_wake(waiter_sched);

    return 0;
}
