#include "apth_barrier.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_sync_waiter.h"
#include "internal/apth_event.h"
#include "internal/apth_sched.h"
#include "internal/apth_dedicated.h"
#include "utils/apth_errno.h"
#include "utils/lll.inline.h"

int apth_barrier_init(apth_barrier_t *barrier, const void *attr, unsigned int count)
{
    (void)attr;
    if (barrier == NULL || count == 0)
        return EINVAL;

    struct apth_barrier_st *b = APTH_BARRIER_CAST(barrier);

    lll_apth_init(&b->guard);
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

    lll_apth_lock(&b->guard);

    // If threads are still waiting, cannot destroy
    if (b->count > 0 || !list_empty(&b->waiters))
    {
        lll_apth_unlock(&b->guard);
        return EBUSY;
    }

    lll_apth_unlock(&b->guard);

    return 0;
}

int apth_barrier_wait(apth_barrier_t *barrier)
{
    if (barrier == NULL)
        return EINVAL;

    struct apth_barrier_st *b = APTH_BARRIER_CAST(barrier);
    apth_t self = CUR_APTH;

    lll_apth_lock(&b->guard);

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

            if (w->th->is_dedicated)
                apth_dedicated_unblock(w->th);
            else
                apth_sched_wake_thread(SCHED_OF(w->th), w->th);
        }

        lll_apth_unlock(&b->guard);
        return APTH_BARRIER_SERIAL_THREAD;
    }

    if (self == NULL)
    {
        unsigned gen = b->generation;
        lll_apth_unlock(&b->guard);
        while (1)
        {
            sched_yield();
            lll_apth_lock(&b->guard);
            if (b->generation != gen)
            {
                lll_apth_unlock(&b->guard);
                return 0;
            }
            lll_apth_unlock(&b->guard);
        }
    }

    // Not the last thread: must wait
    struct apth_sync_waiter w;
    w.th = self;
    w.ev.ev_status = APTH_EV_STATUS_PENDING;
    w.ev.ev_type = APTH_EVENT_TYPE_SYNC;
    w.ev.ev_goal = APTH_GOAL_UNTIL_OCCURRED;
    w.ev.epoll_registered = false;

    // Enqueue waiter
    list_push_back(&b->waiters, &w.elem);

    // Add event to thread's event list.
    // Dedicated threads have no scheduler event manager, so skip this.
    if (!self->is_dedicated)
        apth_event_list_add(&self->event_list, &w.ev);

    lll_apth_unlock(&b->guard);

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
