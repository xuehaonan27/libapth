#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"

int apth_barrier_init(apth_barrier_t *barrier, const void *attr, unsigned int count)
{
    (void)attr;
    if (barrier == NULL || count == 0)
        return EINVAL;

    struct apth_barrier_st *b = APTH_BARRIER_CAST(barrier);

    lll_init(&b->guard);
    b->threshold = count;
    b->count = 0;
    b->generation = 0;
    list_init(&b->waiters);

    return 0;
}

int apth_barrier_destroy(apth_barrier_t *barrier)
{
    if (barrier == NULL)
        return EINVAL;

    struct apth_barrier_st *b = APTH_BARRIER_CAST(barrier);

    lll_lock(&b->guard, "barrier_destroy");

    // If threads are still waiting, cannot destroy
    if (b->count > 0 || !list_empty(&b->waiters))
    {
        lll_unlock(&b->guard, "barrier_destroy");
        return EBUSY;
    }

    lll_unlock(&b->guard, "barrier_destroy");

    return 0;
}

int apth_barrier_wait(apth_barrier_t *barrier)
{
    if (barrier == NULL)
        return EINVAL;

    struct apth_barrier_st *b = APTH_BARRIER_CAST(barrier);
    apth_t self = cur_apth();

    lll_lock(&b->guard, "barrier_wait");

    b->count++;

    if (b->count == b->threshold)
    {
        // Last thread to arrive: reset and wake all
        b->count = 0;
        b->generation++;

        // Wake all waiters
        while (!list_empty(&b->waiters))
        {
            struct list_elem *e = list_pop_front(&b->waiters);
            struct apth_sync_waiter *w = apth_sync_waiter_entry(e);

            w->ev.ev_status = APTH_EV_STATUS_OCCURRED;
            apth_sched_wake(sched_of(w->th));
        }

        lll_unlock(&b->guard, "barrier_wait");
        return APTH_BARRIER_SERIAL_THREAD;
    }

    // Not the last thread: must wait
    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_COND;
    w.ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    w.ev.epoll_registered = false;

    // Enqueue waiter
    list_push_back(&b->waiters, &w.elem);

    // Add event to thread's event list
    apth_event_list_add(&self->event_list, &w.ev);

    lll_unlock(&b->guard, "barrier_wait_pre_yield");

    submit_desired_state_to(self, APTH_STATE_WAITING, "barrier_wait");
    apth_yield();

    // --- woken up ---
    apth_event_isolate(&w.ev);

    return 0;
}

