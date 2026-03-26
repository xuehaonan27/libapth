#include "apth_sem.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_sync_waiter.h"
#include "internal/apth_event.h"
#include "internal/apth_sched.h"
#include "internal/apth_dedicated.h"
#include "utils/apth_errno.h"
#include "utils/lll.inline.h"
#include <time.h>
#include <sched.h>

int apth_sem_init(apth_sem_t *sem, int pshared, unsigned int value)
{
    (void)pshared;
    if (sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);

    lll_apth_init(&s->guard);
    s->value = value;
    list_init(&s->waiters);
    return 0;
}

int apth_sem_destroy(apth_sem_t *sem)
{
    if (sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);

    lll_apth_lock(&s->guard);
    if (!list_empty(&s->waiters))
    {
        lll_apth_unlock(&s->guard);
        return EBUSY;
    }
    lll_apth_unlock(&s->guard);

    return 0;
}

int apth_sem_wait(apth_sem_t *sem)
{
    if (sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);
    apth_t self = CUR_APTH;

    lll_apth_lock(&s->guard);

    // Fast path: semaphore has available count
    if (s->value > 0)
    {
        s->value--;
        lll_apth_unlock(&s->guard);
        return 0;
    }

    // Slow path: must block
    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_SYNC;
    w.ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    w.ev.epoll_registered = false;

    // Enqueue waiter
    list_push_back(&s->waiters, &w.elem);

    // Add event to thread's event list.
    // Dedicated threads have no scheduler event manager, so skip this.
    if (!self->is_dedicated)
        apth_event_list_add(&self->event_list, &w.ev);

    lll_apth_unlock(&s->guard);

    if (self->is_dedicated)
    {
        // Dedicated threads block on their wake eventfd instead of yielding
        while (atomic_load_acquire(&w.ev.ev_status) != APTH_EV_STATUS_OCCURRED)
            apth_dedicated_block(self);
    }
    else
    {
        atomic_store_release(&self->state, APTH_STATE_WAITING);
        self->yield_reason = APTH_YIELD_REASON_WAIT;
        apth_yield();

        // --- woken up ---
        apth_event_isolate(&w.ev);
    }

    return 0;
}

int apth_sem_timedwait(apth_sem_t *sem, const struct timespec *abstime)
{
    if (sem == NULL)
        return EINVAL;
    if (abstime == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);
    apth_t self = CUR_APTH;

    lll_apth_lock(&s->guard);

    // Fast path: semaphore has available count
    if (s->value > 0)
    {
        s->value--;
        lll_apth_unlock(&s->guard);
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

    // Enqueue waiter
    list_push_back(&s->waiters, &w.elem);

    // Add events to event list.
    // Dedicated threads have no scheduler event manager, so skip this.
    if (!self->is_dedicated)
    {
        apth_event_list_add(&self->event_list, &w.ev);
        apth_event_list_add(&self->event_list, &timer_ev);
    }

    lll_apth_unlock(&s->guard);

    if (self->is_dedicated)
    {
        // Dedicated threads: poll with timeout via eventfd + clock check
        struct timespec now;
        while (atomic_load_acquire(&w.ev.ev_status) != APTH_EV_STATUS_OCCURRED)
        {
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > abstime->tv_sec ||
                (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec))
                break; // Timed out
            sched_yield(); // Brief yield to OS, then re-check
        }
    }
    else
    {
        atomic_store_release(&self->state, APTH_STATE_WAITING);
        self->yield_reason = APTH_YIELD_REASON_WAIT;
        apth_yield();

        // --- woken up ---
        apth_event_isolate(&w.ev);
        apth_event_isolate(&timer_ev);
    }

    // Resolve race: post vs timeout
    int ret = 0;
    lll_apth_lock(&s->guard);

    if (w.ev.ev_status != APTH_EV_STATUS_OCCURRED)
    {
        // Timer fired first; we're still in the waiters list
        list_remove(&w.elem);
        ret = ETIMEDOUT;
    }

    lll_apth_unlock(&s->guard);

    return ret;
}

int apth_sem_trywait(apth_sem_t *sem)
{
    if (sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);

    // Use trylock for non-blocking guard acquisition
    if (lll_apth_trylock(&s->guard) != 0)
        return EAGAIN;

    if (s->value == 0)
    {
        lll_apth_unlock(&s->guard);
        return EAGAIN;
    }

    s->value--;
    lll_apth_unlock(&s->guard);

    return 0;
}

int apth_sem_post(apth_sem_t *sem)
{
    if (sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);

    lll_apth_lock(&s->guard);

    // If there are waiters, wake one
    if (!list_empty(&s->waiters))
    {
        struct list_elem *e = list_pop_front(&s->waiters);
        struct apth_sync_waiter *w = apth_sync_waiter_entry(e);

        // Direct wakeup
        w->ev.ev_status = APTH_EV_STATUS_OCCURRED;

        if (w->th->is_dedicated)
        {
            lll_apth_unlock(&s->guard);
            apth_dedicated_unblock(w->th);
        }
        else
        {
            apth_sched_t ws = SCHED_OF(w->th);
            lll_apth_unlock(&s->guard);
            apth_sched_wake(ws);
        }
        return 0;
    }

    // No waiters, increment value
    s->value++;
    lll_apth_unlock(&s->guard);

    return 0;
}

int apth_sem_getvalue(apth_sem_t *sem, int *sval)
{
    if (sem == NULL || sval == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);

    lll_apth_lock(&s->guard);
    *sval = (int)s->value;
    lll_apth_unlock(&s->guard);

    return 0;
}
