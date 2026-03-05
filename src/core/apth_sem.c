#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"

int apth_sem_init(apth_sem_t *sem, int pshared, unsigned int value)
{
    (void)pshared;
    if (sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);

    lll_init(&s->guard);
    s->value = value;
    list_init(&s->waiters);
    return 0;
}

int apth_sem_destroy(apth_sem_t *sem)
{
    if (sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);

    lll_lock(&s->guard, "sem_destroy");
    if (!list_empty(&s->waiters))
    {
        lll_unlock(&s->guard, "sem_destroy");
        return EBUSY;
    }
    lll_unlock(&s->guard, "sem_destroy");

    return 0;
}

int apth_sem_wait(apth_sem_t *sem)
{
    if (sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);
    apth_t self = cur_apth();

    lll_lock(&s->guard, "sem_wait");

    // Fast path: semaphore has available count
    if (s->value > 0)
    {
        s->value--;
        lll_unlock(&s->guard, "sem_wait");
        return 0;
    }

    // Slow path: must block
    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_COND;
    w.ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    w.ev.epoll_registered = false;

    // Enqueue waiter
    list_push_back(&s->waiters, &w.elem);

    // Add event to thread's event list
    apth_event_list_add(&self->event_list, &w.ev);

    lll_unlock(&s->guard, "sem_wait_pre_yield");

    submit_desired_state_to(self, APTH_STATE_WAITING, "sem_wait");
    apth_yield();

    // --- woken up ---
    apth_event_isolate(&w.ev);

    return 0;
}

int apth_sem_timedwait(apth_sem_t *sem, const struct timespec *abstime)
{
    if (sem == NULL)
        return EINVAL;
    if (abstime == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);
    apth_t self = cur_apth();

    lll_lock(&s->guard, "sem_timedwait");

    // Fast path: semaphore has available count
    if (s->value > 0)
    {
        s->value--;
        lll_unlock(&s->guard, "sem_timedwait");
        return 0;
    }

    // Slow path: must block with timeout
    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_COND;
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

    // Add BOTH events to event list
    apth_event_list_add(&self->event_list, &w.ev);
    apth_event_list_add(&self->event_list, &timer_ev);

    lll_unlock(&s->guard, "sem_timedwait_pre_yield");

    submit_desired_state_to(self, APTH_STATE_WAITING, "sem_timedwait");
    apth_yield();

    // --- woken up ---
    apth_event_isolate(&w.ev);
    apth_event_isolate(&timer_ev);

    // Resolve race: post vs timeout
    int ret = 0;
    lll_lock(&s->guard, "sem_timedwait_post");

    if (w.ev.ev_status != APTH_EV_STATUS_OCCURRED)
    {
        // Timer fired first; we're still in the waiters list
        list_remove(&w.elem);
        ret = ETIMEDOUT;
    }

    lll_unlock(&s->guard, "sem_timedwait_post");

    return ret;
}

int apth_sem_trywait(apth_sem_t *sem)
{
    if (sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);

    lll_lock(&s->guard, "sem_trywait");

    if (s->value == 0)
    {
        lll_unlock(&s->guard, "sem_trywait");
        return EAGAIN;
    }

    s->value--;
    lll_unlock(&s->guard, "sem_trywait");

    return 0;
}

int apth_sem_post(apth_sem_t *sem)
{
    if (sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);

    lll_lock(&s->guard, "sem_post");

    // If there are waiters, wake one
    if (!list_empty(&s->waiters))
    {
        struct list_elem *e = list_pop_front(&s->waiters);
        struct apth_sync_waiter *w = apth_sync_waiter_entry(e);

        // Direct wakeup
        w->ev.ev_status = APTH_EV_STATUS_OCCURRED;
        apth_sched_t ws = sched_of(w->th);

        lll_unlock(&s->guard, "sem_post");
        apth_sched_wake(ws);
        return 0;
    }

    // No waiters, increment value
    s->value++;
    lll_unlock(&s->guard, "sem_post");

    return 0;
}

int apth_sem_getvalue(apth_sem_t *sem, int *sval)
{
    if (sem == NULL || sval == NULL)
        return EINVAL;

    struct apth_sem_st *s = APTH_SEM_CAST(sem);

    lll_lock(&s->guard, "sem_getvalue");
    *sval = (int)s->value;
    lll_unlock(&s->guard, "sem_getvalue");

    return 0;
}

