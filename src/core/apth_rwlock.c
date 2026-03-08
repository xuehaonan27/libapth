#include "apth_rwlock.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_sync_waiter.h"
#include "internal/apth_event.h"
#include "internal/apth_sched.h"
#include "utils/apth_errno.h"
#include "utils/lll_new.inline.h"

int apth_rwlock_init(apth_rwlock_t *rwlock, const void *attr)
{
    (void)attr;
    if (rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = APTH_RWLOCK_CAST(rwlock);

    lll_apth_init(&rw->guard);
    rw->readers = 0;
    rw->writers = 0;
    rw->waiting_writers = 0;
    list_init(&rw->rd_waiters);
    list_init(&rw->wr_waiters);

    return 0;
}

int apth_rwlock_destroy(apth_rwlock_t *rwlock)
{
    if (rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = APTH_RWLOCK_CAST(rwlock);

    lll_apth_lock(&rw->guard);

    if (rw->readers > 0 || rw->writers > 0 ||
        !list_empty(&rw->rd_waiters) || !list_empty(&rw->wr_waiters))
    {
        lll_apth_unlock(&rw->guard);
        return EBUSY;
    }

    lll_apth_unlock(&rw->guard);

    return 0;
}

int apth_rwlock_rdlock(apth_rwlock_t *rwlock)
{
    if (rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = APTH_RWLOCK_CAST(rwlock);
    apth_t self = CUR_APTH;

    lll_apth_lock(&rw->guard);

    // Fast path: no writers and no waiting writers
    if (rw->writers == 0 && rw->waiting_writers == 0)
    {
        rw->readers++;
        lll_apth_unlock(&rw->guard);
        return 0;
    }

    // Slow path: must block
    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_SYNC;
    w.ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    w.ev.epoll_registered = false;

    // Enqueue as reader waiter
    list_push_back(&rw->rd_waiters, &w.elem);

    // Add event to thread's event list
    apth_event_list_add(&self->event_list, &w.ev);

    lll_apth_unlock(&rw->guard);

    atomic_store_release(&self->state, APTH_STATE_WAITING);
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    // --- woken up ---
    apth_event_isolate(&w.ev);

    return 0;
}

int apth_rwlock_timedrdlock(apth_rwlock_t *rwlock, const struct timespec *abstime)
{
    if (rwlock == NULL)
        return EINVAL;
    if (abstime == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = APTH_RWLOCK_CAST(rwlock);
    apth_t self = CUR_APTH;

    lll_apth_lock(&rw->guard);

    // Fast path: no writers and no waiting writers
    if (rw->writers == 0 && rw->waiting_writers == 0)
    {
        rw->readers++;
        lll_apth_unlock(&rw->guard);
        return 0;
    }

    // Slow path: must block with timeout
    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_SYNC;
    w.ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    w.ev.epoll_registered = false;

    // Prepare TIME event
    struct apth_event_st timer_ev;
    timer_ev.ev_status = APTH_EV_STATUS_PENDING;
    timer_ev.ev_type = APTH_EVENT_TYPE_TIME;
    timer_ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    timer_ev.epoll_registered = false;
    timer_ev.ev_args.TIME.tv.tv_sec = abstime->tv_sec;
    timer_ev.ev_args.TIME.tv.tv_usec = abstime->tv_nsec / 1000;

    // Enqueue as reader waiter
    list_push_back(&rw->rd_waiters, &w.elem);

    // Add BOTH events to event list
    apth_event_list_add(&self->event_list, &w.ev);
    apth_event_list_add(&self->event_list, &timer_ev);

    lll_apth_unlock(&rw->guard);

    atomic_store_release(&self->state, APTH_STATE_WAITING);
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    // --- woken up ---
    apth_event_isolate(&w.ev);
    apth_event_isolate(&timer_ev);

    // Resolve race: unlock vs timeout
    int ret = 0;
    lll_apth_lock(&rw->guard);

    if (w.ev.ev_status != APTH_EV_STATUS_OCCURRED)
    {
        // Timer fired first; we're still in the waiters list
        list_remove(&w.elem);
        ret = ETIMEDOUT;
    }

    lll_apth_unlock(&rw->guard);

    return ret;
}

int apth_rwlock_tryrdlock(apth_rwlock_t *rwlock)
{
    if (rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = APTH_RWLOCK_CAST(rwlock);

    // Use trylock for non-blocking guard acquisition
    if (lll_apth_trylock(&rw->guard) != 0)
        return EBUSY;

    if (rw->writers > 0 || rw->waiting_writers > 0)
    {
        lll_apth_unlock(&rw->guard);
        return EBUSY;
    }

    rw->readers++;
    lll_apth_unlock(&rw->guard);

    return 0;
}

int apth_rwlock_wrlock(apth_rwlock_t *rwlock)
{
    if (rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = APTH_RWLOCK_CAST(rwlock);
    apth_t self = CUR_APTH;

    lll_apth_lock(&rw->guard);

    // Fast path: no readers and no writers
    if (rw->readers == 0 && rw->writers == 0)
    {
        rw->writers = 1;
        lll_apth_unlock(&rw->guard);
        return 0;
    }

    // Slow path: must block
    rw->waiting_writers++;

    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_SYNC;
    w.ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    w.ev.epoll_registered = false;

    // Enqueue as writer waiter
    list_push_back(&rw->wr_waiters, &w.elem);

    // Add event to thread's event list
    apth_event_list_add(&self->event_list, &w.ev);

    lll_apth_unlock(&rw->guard);

    atomic_store_release(&self->state, APTH_STATE_WAITING);
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    // --- woken up ---
    apth_event_isolate(&w.ev);

    return 0;
}

int apth_rwlock_timedwrlock(apth_rwlock_t *rwlock, const struct timespec *abstime)
{
    if (rwlock == NULL)
        return EINVAL;
    if (abstime == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = APTH_RWLOCK_CAST(rwlock);
    apth_t self = CUR_APTH;

    lll_apth_lock(&rw->guard);

    // Fast path: no readers and no writers
    if (rw->readers == 0 && rw->writers == 0)
    {
        rw->writers = 1;
        lll_apth_unlock(&rw->guard);
        return 0;
    }

    // Slow path: must block with timeout
    rw->waiting_writers++;

    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_SYNC;
    w.ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    w.ev.epoll_registered = false;

    // Prepare TIME event
    struct apth_event_st timer_ev;
    timer_ev.ev_status = APTH_EV_STATUS_PENDING;
    timer_ev.ev_type = APTH_EVENT_TYPE_TIME;
    timer_ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    timer_ev.epoll_registered = false;
    timer_ev.ev_args.TIME.tv.tv_sec = abstime->tv_sec;
    timer_ev.ev_args.TIME.tv.tv_usec = abstime->tv_nsec / 1000;

    // Enqueue as writer waiter
    list_push_back(&rw->wr_waiters, &w.elem);

    // Add BOTH events to event list
    apth_event_list_add(&self->event_list, &w.ev);
    apth_event_list_add(&self->event_list, &timer_ev);

    lll_apth_unlock(&rw->guard);

    atomic_store_release(&self->state, APTH_STATE_WAITING);
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    // --- woken up ---
    apth_event_isolate(&w.ev);
    apth_event_isolate(&timer_ev);

    // Resolve race: unlock vs timeout
    int ret = 0;
    lll_apth_lock(&rw->guard);

    if (w.ev.ev_status != APTH_EV_STATUS_OCCURRED)
    {
        // Timer fired first; we're still in the waiters list
        list_remove(&w.elem);
        rw->waiting_writers--;
        ret = ETIMEDOUT;
    }

    lll_apth_unlock(&rw->guard);

    return ret;
}

int apth_rwlock_trywrlock(apth_rwlock_t *rwlock)
{
    if (rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = APTH_RWLOCK_CAST(rwlock);

    // Use trylock for non-blocking guard acquisition
    if (lll_apth_trylock(&rw->guard) != 0)
        return EBUSY;

    if (rw->readers > 0 || rw->writers > 0)
    {
        lll_apth_unlock(&rw->guard);
        return EBUSY;
    }

    rw->writers = 1;
    lll_apth_unlock(&rw->guard);

    return 0;
}

int apth_rwlock_unlock(apth_rwlock_t *rwlock)
{
    if (rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = APTH_RWLOCK_CAST(rwlock);

    lll_apth_lock(&rw->guard);

    if (rw->writers > 0)
    {
        // Writer releasing
        rw->writers = 0;

        // Write-preferring: wake a writer if any, otherwise wake all readers
        if (!list_empty(&rw->wr_waiters))
        {
            struct list_elem *e = list_pop_front(&rw->wr_waiters);
            struct apth_sync_waiter *w = apth_sync_waiter_entry(e);

            rw->waiting_writers--;
            rw->writers = 1;
            w->ev.ev_status = APTH_EV_STATUS_OCCURRED;
            apth_sched_t ws = SCHED_OF(w->th);

            lll_apth_unlock(&rw->guard);
            apth_sched_wake(ws);
            return 0;
        }

        // No waiting writers, wake all readers
        while (!list_empty(&rw->rd_waiters))
        {
            struct list_elem *e = list_pop_front(&rw->rd_waiters);
            struct apth_sync_waiter *w = apth_sync_waiter_entry(e);

            rw->readers++;
            w->ev.ev_status = APTH_EV_STATUS_OCCURRED;
            apth_sched_wake(SCHED_OF(w->th));
        }

        lll_apth_unlock(&rw->guard);
        return 0;
    }
    else if (rw->readers > 0)
    {
        // Reader releasing
        rw->readers--;

        if (rw->readers == 0 && !list_empty(&rw->wr_waiters))
        {
            // Last reader, wake a writer
            struct list_elem *e = list_pop_front(&rw->wr_waiters);
            struct apth_sync_waiter *w = apth_sync_waiter_entry(e);

            rw->waiting_writers--;
            rw->writers = 1;
            w->ev.ev_status = APTH_EV_STATUS_OCCURRED;
            apth_sched_t ws = SCHED_OF(w->th);

            lll_apth_unlock(&rw->guard);
            apth_sched_wake(ws);
            return 0;
        }

        lll_apth_unlock(&rw->guard);
        return 0;
    }

    lll_apth_unlock(&rw->guard);
    return 0;
}
