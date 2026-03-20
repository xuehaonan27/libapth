#include "apth_event.h"
#include "hook_libc/hooked_funcs.h"
#include "internal/apth_cancel.h"
#include "internal/apth_fd.h"
#include "internal/apth_fd_slot.h"
#include "internal/apth_epoll_waiter.h"
#include "internal/apth_reactor.h"
#include "internal/apth_signal.h"
#include "internal/apth_state.h"
#include "internal/apth_time.h"
#include "utils/lll.inline.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include "utils/atomic_wrapper.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

APTH_INTERNAL bool apth_state_matches_event_goal(apth_state_t state, apth_goal_t goal)
{
    return ((int)state == (int)goal);
}

// ==================== Inline poller (default, no reactor) ====================
#ifndef APTH_USE_REACTOR

// Waiter pool (per-scheduler)
static struct apth_epoll_waiter *alloc_waiter(apth_sched_t sched)
{
    if (!list_empty(&sched->free_waiters))
    {
        struct list_elem *e = list_pop_front(&sched->free_waiters);
        return apth_epoll_waiter_list_entry(e);
    }
    // Pool exhausted, fall back to malloc
    sched->waiter_pool_allocated++;
    return (struct apth_epoll_waiter *)malloc(sizeof(struct apth_epoll_waiter));
}

static void free_waiter(apth_sched_t sched, struct apth_epoll_waiter *w)
{
    if (w >= sched->waiter_pool && w < sched->waiter_pool + sched->waiter_pool_size)
        list_push_back(&sched->free_waiters, &w->elem);
    else
    {
        free(w);
        sched->waiter_pool_allocated--;
    }
}

// Ensure fd_slots capacity
static void sched_ensure_fd_slot_capacity(apth_sched_t sched, int fd)
{
    if (fd < sched->fd_slot_capacity)
        return;

    int new_cap = sched->fd_slot_capacity;
    while (new_cap <= fd)
        new_cap *= 2;

    struct apth_epoll_fd_slot *new_slots = (struct apth_epoll_fd_slot *)calloc(new_cap, sizeof(struct apth_epoll_fd_slot));
    if (new_slots == NULL)
        return; // Silently fail; caller will handle

    // Copy existing slots
    memcpy(new_slots, sched->fd_slots, sched->fd_slot_capacity * sizeof(struct apth_epoll_fd_slot));

    // Fix up list pointers: active_fd_slots contains elem from the old array.
    // We need to re-link them from the new array.
    // Strategy: rebuild list by scanning all slots.
    list_init(&sched->active_fd_slots);
    sched->active_fd_count = 0;

    for (int i = 0; i < sched->fd_slot_capacity; i++)
    {
        // Preserve waiter lists by re-initializing the list head to point to new location
        if (new_slots[i].waiter_count > 0)
        {
            struct list *old_list = &sched->fd_slots[i].waiters;
            struct list *new_list = &new_slots[i].waiters;
            if (!list_empty(old_list))
            {
                new_list->head.next = old_list->head.next;
                new_list->tail.prev = old_list->tail.prev;
                new_list->head.next->prev = &new_list->head;
                new_list->tail.prev->next = &new_list->tail;
            }
            else
            {
                list_init(new_list);
            }
        }

        if (new_slots[i].registered)
        {
            list_push_back(&sched->active_fd_slots, &new_slots[i].elem);
            sched->active_fd_count++;
        }
    }

    // Initialize new slots
    for (int i = sched->fd_slot_capacity; i < new_cap; i++)
    {
        new_slots[i].fd = i;
        list_init(&new_slots[i].waiters);
        new_slots[i].waiter_count = 0;
        new_slots[i].registered = false;
    }

    free(sched->fd_slots);
    sched->fd_slots = new_slots;
    sched->fd_slot_capacity = new_cap;
}

// Add waiter with edge-triggered permanent registration
static int epoll_map_add_waiter(apth_sched_t sched, int fd, apth_t th, apth_event_t ev)
{
    if (fd < 0) return -1;
    if (ev->epoll_registered) return 0;

    sched_ensure_fd_slot_capacity(sched, fd);
    if (fd >= sched->fd_slot_capacity) return -1;

    struct apth_epoll_fd_slot *slot = &sched->fd_slots[fd];

    struct apth_epoll_waiter *w = alloc_waiter(sched);
    if (!w) return -1;
    w->th = th;
    w->ev = ev;
    ev->epoll_waiter = w;

    list_push_back(&slot->waiters, &w->elem);
    slot->waiter_count++;

    // Edge-triggered registration. We always attempt epoll_ctl(ADD):
    //  - If the FD was never registered: ADD succeeds, slot is now registered.
    //  - If the FD is still registered (same kernel FD): ADD returns EEXIST, no-op.
    //  - If the old FD was closed and the number was reused by a new FD:
    //    the kernel auto-removed the old FD on close(), so ADD succeeds for
    //    the new FD. This handles FD number reuse correctly.
    {
        struct epoll_event ee;
        ee.events = EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLET;
        ee.data.fd = fd;
        int rc = epoll_ctl(sched->epoll_fd, EPOLL_CTL_ADD, fd, &ee);
        if (rc < 0 && errno != EEXIST)
        {
            // Real error (not "already registered")
            list_remove(&w->elem);
            slot->waiter_count--;
            ev->epoll_waiter = NULL;
            free_waiter(sched, w);
            return -1;
        }
        if (rc == 0 && !slot->registered)
        {
            // Newly registered
            slot->registered = true;
            list_push_back(&sched->active_fd_slots, &slot->elem);
            sched->active_fd_count++;
        }
    }

    ev->epoll_registered = true;
    return 0;
}

// Remove waiter (do NOT unregister from epoll -- permanent registration)
static void epoll_map_remove_waiter(apth_sched_t sched, int fd, apth_t th __attribute__((unused)), apth_event_t ev)
{
    if (fd < 0 || fd >= sched->fd_slot_capacity) return;
    if (!ev->epoll_registered) return;

    struct apth_epoll_fd_slot *slot = &sched->fd_slots[fd];
    struct apth_epoll_waiter *w = ev->epoll_waiter;
    if (w) {
        list_remove(&w->elem);
        free_waiter(sched, w);
        ev->epoll_waiter = NULL;
        slot->waiter_count--;
    }
    // Do NOT call epoll_ctl(DEL) -- permanent registration
    ev->epoll_registered = false;
}

// Wake waiters when epoll reports readiness
static void epoll_map_wake_fd(apth_sched_t sched, int fd, uint32_t revents,
                              apth_t *wake_batch, int *wake_count, int wake_max)
{
    if (fd < 0 || fd >= sched->fd_slot_capacity) return;
    struct apth_epoll_fd_slot *slot = &sched->fd_slots[fd];

    struct list_elem *e = list_begin(&slot->waiters);
    while (e != list_end(&slot->waiters))
    {
        struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
        struct list_elem *next = list_next(e);

        bool matched = false;
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE) && (revents & EPOLLIN))
            matched = true;
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE) && (revents & EPOLLOUT))
            matched = true;
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION) && (revents & EPOLLPRI))
            matched = true;
        if (revents & (EPOLLERR | EPOLLHUP))
            matched = true;

        if (matched)
        {
            w->ev->ev_status = APTH_EV_STATUS_OCCURRED;
            w->ev->epoll_waiter = NULL;
            w->ev->epoll_registered = false;

            if (!w->th->wake_pending && *wake_count < wake_max)
            {
                w->th->wake_pending = true;
                wake_batch[(*wake_count)++] = w->th;
            }

            list_remove(&w->elem);
            slot->waiter_count--;
            free_waiter(sched, w);
        }
        e = next;
    }
}

// Fail all waiters for a closed FD
static void epoll_map_fail_all_waiters_for_fd(apth_sched_t sched, int fd)
{
    if (fd < 0 || fd >= sched->fd_slot_capacity)
        return;

    struct apth_epoll_fd_slot *slot = &sched->fd_slots[fd];
    if (slot->waiter_count == 0)
        return;

    struct list_elem *e = list_begin(&slot->waiters);
    while (e != list_end(&slot->waiters))
    {
        struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
        struct list_elem *next = list_next(e);

        w->ev->ev_status = APTH_EV_STATUS_FAILED;
        w->ev->epoll_registered = false;
        w->ev->epoll_waiter = NULL;

        list_remove(&w->elem);
        free_waiter(sched, w);
        e = next;
    }

    slot->waiter_count = 0;

    if (slot->registered)
    {
        epoll_ctl(sched->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        slot->registered = false;
        list_remove(&slot->elem);
        sched->active_fd_count--;
    }
}

// Process pending fd close notifications
static void sched_process_pending_fd_closes(apth_sched_t sched)
{
    if (atomic_load_acquire(&sched->pending_fd_close_count) <= 0)
        return;

    int local_fds[APTH_PENDING_FD_CLOSE_MAX];
    int local_count;

    lll_internal_lock(&sched->pending_fd_close_lock);
    local_count = atomic_load_acquire(&sched->pending_fd_close_count);
    if (local_count > APTH_PENDING_FD_CLOSE_MAX)
        local_count = APTH_PENDING_FD_CLOSE_MAX;
    memcpy(local_fds, sched->pending_fd_close_fds, local_count * sizeof(int));
    atomic_store_release(&sched->pending_fd_close_count, 0);
    lll_internal_unlock(&sched->pending_fd_close_lock);

    for (int i = 0; i < local_count; i++)
        epoll_map_fail_all_waiters_for_fd(sched, local_fds[i]);
}

#endif // !APTH_USE_REACTOR

// ==================== Event manager ====================

APTH_INTERNAL void apth_sched_eventmanager_epoll(apth_sched_t sched, apth_time_t *now, bool dopoll)
{
    apth_debug("enter in %s mode", dopoll ? "polling" : "waiting");

    for (;;)
    {
        bool loop_repeat = false;

        // Phase 0: process pending fd close notifications
#ifndef APTH_USE_REACTOR
        sched_process_pending_fd_closes(sched);
#endif

        // ==================== Phase 1: traverse waiting queue ====================

        apth_time_t nexttimer_value;
        apth_time_set(&nexttimer_value, APTH_TIME_ZERO);
        apth_event_t nexttimer_ev = APTH_EVENT_NULL;
        apth_t nexttimer_th = APTH_NULL;
        bool has_timer = false;
        size_t notified_ths = 0;

#define MAX_WAKE_BATCH 512
        apth_t wake_batch[MAX_WAKE_BATCH];
        int wake_count = 0;

        lll_internal_lock(&THQUEUE(sched, waiting)->th_list_lock);

        FOR_ELEMENT_IN_LIST(THQUEUE(sched, waiting)->th_list, e)
        {
            apth_t th = apth_t_list_entry(e);
            bool any_occurred = false;

            // Check cancelation request
            if (atomic_load_acquire(&th->cancelreq))
                any_occurred = true;

            if (list_empty(&th->event_list))
                goto check_wake;

            FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
            {
                apth_event_t event = apth_event_t_list_entry(ev_e);
                if (event->ev_status != APTH_EV_STATUS_PENDING)
                {
                    // Already marked by reactor/inline poller or previous pass
                    any_occurred = true;
                    continue;
                }

                switch (event->ev_type)
                {
                case APTH_EVENT_TYPE_FD:
                    // Fallback registration for edge cases (e.g., failed eager registration).
                    if (!event->epoll_registered)
                    {
#ifdef APTH_USE_REACTOR
                        if (apth_reactor_add_waiter(sched, event->ev_args.FD.fd, th, event) < 0)
#else
                        if (epoll_map_add_waiter(sched, event->ev_args.FD.fd, th, event) < 0)
#endif
                        {
                            event->ev_status = APTH_EV_STATUS_FAILED;
                            any_occurred = true;
                        }
                    }
                    break;

                case APTH_EVENT_TYPE_SELECT:
                    {
                        struct timeval zero_tv = {0, 0};
                        fd_set trfds, twfds, tefds;
                        fd_set *prfds = NULL, *pwfds = NULL, *pefds = NULL;
                        if (event->ev_args.SELECT.rfds)
                        {
                            memcpy(&trfds, event->ev_args.SELECT.rfds, sizeof(fd_set));
                            prfds = &trfds;
                        }
                        if (event->ev_args.SELECT.wfds)
                        {
                            memcpy(&twfds, event->ev_args.SELECT.wfds, sizeof(fd_set));
                            pwfds = &twfds;
                        }
                        if (event->ev_args.SELECT.efds)
                        {
                            memcpy(&tefds, event->ev_args.SELECT.efds, sizeof(fd_set));
                            pefds = &tefds;
                        }

                        int rc;
                        while ((rc = apth_func_raw(select)(event->ev_args.SELECT.nfd, prfds, pwfds, pefds, &zero_tv)) < 0 && errno == EINTR)
                            ;
                        if (rc > 0)
                        {
                            int n = apth_util_fds_select(event->ev_args.SELECT.nfd,
                                                         event->ev_args.SELECT.rfds, prfds,
                                                         event->ev_args.SELECT.wfds, pwfds,
                                                         event->ev_args.SELECT.efds, pefds);
                            if (event->ev_args.SELECT.n)
                                *(event->ev_args.SELECT.n) = n;
                            event->ev_status = APTH_EV_STATUS_OCCURRED;
                            any_occurred = true;
                        }
                        else if (rc < 0)
                        {
                            event->ev_status = APTH_EV_STATUS_FAILED;
                            any_occurred = true;
                        }
                    }
                    break;

                case APTH_EVENT_TYPE_SIGS:
                    if (th->sigpendcnt > 0)
                    {
                        lll_internal_lock(&th->siglock);
                        for (int sig = 1; sig < APTH_NSIG; sig++)
                        {
                            if (sigismember(event->ev_args.SIGS.sigs, sig) &&
                                sigismember(&th->sigpending, sig))
                            {
                                if (event->ev_args.SIGS.sig)
                                    *(event->ev_args.SIGS.sig) = sig;
                                sigdelset(&th->sigpending, sig);
                                th->sigpendcnt--;
                                event->ev_status = APTH_EV_STATUS_OCCURRED;
                                any_occurred = true;
                                break;
                            }
                        }
                        lll_internal_unlock(&th->siglock);
                    }
                    break;

                case APTH_EVENT_TYPE_TIME:
                    if (apth_time_cmp(&event->ev_args.TIME.tv, now) < 0)
                    {
                        event->ev_status = APTH_EV_STATUS_OCCURRED;
                        any_occurred = true;
                    }
                    else
                    {
                        if (!has_timer || apth_time_cmp(&event->ev_args.TIME.tv, &nexttimer_value) < 0)
                        {
                            apth_time_set(&nexttimer_value, &event->ev_args.TIME.tv);
                            nexttimer_ev = event;
                            nexttimer_th = th;
                            has_timer = true;
                        }
                    }
                    break;

                case APTH_EVENT_TYPE_TID:
                    if ((event->ev_args.TID.tid == NULL && thqueue_size(THQUEUE(sched, terminated)) != 0) ||
                        (event->ev_args.TID.tid != NULL &&
                         apth_state_matches_event_goal(atomic_load_acquire(&event->ev_args.TID.tid->state), event->ev_goal)))
                    {
                        event->ev_status = APTH_EV_STATUS_OCCURRED;
                        any_occurred = true;
                    }
                    break;

                case APTH_EVENT_TYPE_FUNC:
                    if (event->ev_args.FUNC.func(event->ev_args.FUNC.arg))
                    {
                        event->ev_status = APTH_EV_STATUS_OCCURRED;
                        any_occurred = true;
                    }
                    else
                    {
                        apth_time_t tv;
                        apth_time_set(&tv, now);
                        apth_time_add(&tv, &event->ev_args.FUNC.tv);
                        if (!has_timer || apth_time_cmp(&tv, &nexttimer_value) < 0)
                        {
                            apth_time_set(&nexttimer_value, &tv);
                            nexttimer_ev = event;
                            nexttimer_th = th;
                            has_timer = true;
                        }
                    }
                    break;

                default:
                    break;
                }
            }

        check_wake:
            if (any_occurred)
            {
                notified_ths++;
                if (wake_count < MAX_WAKE_BATCH)
                    wake_batch[wake_count++] = th;
            }
        }

        lll_internal_unlock(&THQUEUE(sched, waiting)->th_list_lock);

        // Transfer waked threads from waiting to waked queue.
        // Remove pending FD registrations for waked threads.
        do
        {
            for (int i = 0; i < wake_count; i++)
            {
                apth_t th = wake_batch[i];
                // Remove any remaining FD events for this thread
                FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
                {
                    apth_event_t event = apth_event_t_list_entry(ev_e);
                    if (event->ev_type == APTH_EVENT_TYPE_FD && event->epoll_registered)
                    {
#ifdef APTH_USE_REACTOR
                        apth_reactor_remove_waiter(sched, event->ev_args.FD.fd, th, event);
#else
                        epoll_map_remove_waiter(sched, event->ev_args.FD.fd, th, event);
#endif
                    }
                }
                atomic_store_release(&th->state, APTH_STATE_WAKED);
                transfer_th(th, THQUEUE(sched, waiting), THQUEUE(sched, waked));
            }

            if (wake_count == MAX_WAKE_BATCH)
            {
                wake_count = 0;
                lll_internal_lock(&THQUEUE(sched, waiting)->th_list_lock);
                FOR_ELEMENT_IN_LIST(THQUEUE(sched, waiting)->th_list, e)
                {
                    apth_t th = apth_t_list_entry(e);
                    bool any_occurred = atomic_load_acquire(&th->cancelreq);
                    if (!any_occurred)
                    {
                        FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
                        {
                            apth_event_t event = apth_event_t_list_entry(ev_e);
                            if (event->ev_status != APTH_EV_STATUS_PENDING)
                            {
                                any_occurred = true;
                                break;
                            }
                        }
                    }
                    if (any_occurred && wake_count < MAX_WAKE_BATCH)
                        wake_batch[wake_count++] = th;
                }
                lll_internal_unlock(&THQUEUE(sched, waiting)->th_list_lock);
                notified_ths += wake_count;
            }
            else
            {
                break;
            }
        } while (wake_count > 0);

        // If there are waked threads, return immediately to dispatch them
        if (notified_ths > 0)
            dopoll = true;

        // ==================== Phase 2: epoll_wait ====================

        int timeout_ms;
        if (dopoll)
        {
            // We have ready work, but still do a quick non-blocking poll
            // to avoid starving I/O-waiting threads. timeout=0 is just
            // a syscall check, no blocking.
            timeout_ms = 0;
        }
        else if (has_timer)
        {
            apth_time_t diff;
            apth_time_set(&diff, &nexttimer_value);
            apth_time_sub(&diff, now);
            timeout_ms = (int)(diff.tv_sec * 1000 + diff.tv_usec / 1000);
            if (timeout_ms < 0)
                timeout_ms = 0;
            if (timeout_ms > 60000)
                timeout_ms = 60000;
        }
        else
        {
            // No timer, no ready work: block briefly on epoll.
            timeout_ms = 10;
        }

        if (timeout_ms >= 0)
        {
#ifndef APTH_USE_REACTOR
            // Inline poller: epoll_wait monitors wake_eventfd + ALL FDs
            struct epoll_event ep_events[64];
            int nready = epoll_wait(sched->epoll_fd, ep_events, 64, timeout_ms);

            if (nready > 0)
            {
                apth_t fd_wake_batch[MAX_WAKE_BATCH];
                int fd_wake_count = 0;

                for (int i = 0; i < nready; i++)
                {
                    if (ep_events[i].data.fd == sched->wake_eventfd)
                    {
                        // Drain the wake eventfd
                        uint64_t val;
                        ssize_t __ignored = apth_func_raw(read)(sched->wake_eventfd, &val, sizeof(val));
                        (void)__ignored;
                    }
                    else
                    {
                        // FD event: wake matching waiters
                        epoll_map_wake_fd(sched, ep_events[i].data.fd, ep_events[i].events,
                                          fd_wake_batch, &fd_wake_count, MAX_WAKE_BATCH);
                    }
                }

                // Transfer FD-waked threads
                for (int i = 0; i < fd_wake_count; i++)
                {
                    apth_t th = fd_wake_batch[i];
                    th->wake_pending = false;

                    // Remove remaining pending FD events
                    FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
                    {
                        apth_event_t event = apth_event_t_list_entry(ev_e);
                        if (event->ev_type == APTH_EVENT_TYPE_FD &&
                            event->ev_status == APTH_EV_STATUS_PENDING)
                            epoll_map_remove_waiter(sched, event->ev_args.FD.fd, th, event);
                    }

                    atomic_store_release(&th->state, APTH_STATE_WAKED);
                    transfer_th(th, THQUEUE(sched, waiting), THQUEUE(sched, waked));
                }
            }
            else if (nready == 0 && !dopoll && has_timer)
            {
                // Timer might have fired
                if (nexttimer_ev != NULL)
                {
                    if (nexttimer_ev->ev_type == APTH_EVENT_TYPE_FUNC)
                    {
                        loop_repeat = true;
                    }
                    else
                    {
                        nexttimer_ev->ev_status = APTH_EV_STATUS_OCCURRED;
                        apth_debug("[timeout] event occurred for apth \"%s\"", nexttimer_th->name);
                        FOR_ELEMENT_IN_LIST(nexttimer_th->event_list, ev_e)
                        {
                            apth_event_t event = apth_event_t_list_entry(ev_e);
                            if (event->ev_type == APTH_EVENT_TYPE_FD && event->epoll_registered)
                                epoll_map_remove_waiter(sched, event->ev_args.FD.fd, nexttimer_th, event);
                        }
                        atomic_store_release(&nexttimer_th->state, APTH_STATE_WAKED);
                        transfer_th(nexttimer_th, THQUEUE(sched, waiting), THQUEUE(sched, waked));
                    }
                }
            }
#else
            // Reactor path: epoll_wait only monitors wake_eventfd
            struct epoll_event ep_events[4];
            int nready = epoll_wait(sched->epoll_fd, ep_events, 4, timeout_ms);

            if (nready > 0)
            {
                // Drain the wake eventfd
                for (int i = 0; i < nready; i++)
                {
                    if (ep_events[i].data.fd == sched->wake_eventfd)
                    {
                        uint64_t val;
                        ssize_t __ignored = apth_func_raw(read)(sched->wake_eventfd, &val, sizeof(val));
                        (void)__ignored;
                    }
                }
            }
            else if (nready == 0 && !dopoll && has_timer)
            {
                // Timer might have fired
                if (nexttimer_ev != NULL)
                {
                    if (nexttimer_ev->ev_type == APTH_EVENT_TYPE_FUNC)
                    {
                        loop_repeat = true;
                    }
                    else
                    {
                        nexttimer_ev->ev_status = APTH_EV_STATUS_OCCURRED;
                        apth_debug("[timeout] event occurred for apth \"%s\"", nexttimer_th->name);
                        FOR_ELEMENT_IN_LIST(nexttimer_th->event_list, ev_e)
                        {
                            apth_event_t event = apth_event_t_list_entry(ev_e);
                            if (event->ev_type == APTH_EVENT_TYPE_FD && event->epoll_registered)
                                apth_reactor_remove_waiter(sched, event->ev_args.FD.fd, nexttimer_th, event);
                        }
                        atomic_store_release(&nexttimer_th->state, APTH_STATE_WAKED);
                        transfer_th(nexttimer_th, THQUEUE(sched, waiting), THQUEUE(sched, waked));
                    }
                }
            }
#endif // APTH_USE_REACTOR
        }

        if (loop_repeat)
        {
            apth_time_set(now, APTH_TIME_NOW);
            continue;
        }
        else
        {
            break;
        }
    }

    apth_debug("leave");
}

APTH_INTERNAL void apth_event_list_add(struct list *el, apth_event_t ev)
{
    list_push_back(el, &ev->elem);
}

APTH_INTERNAL void apth_event_isolate(apth_event_t ev)
{
    list_remove(&ev->elem);
}

APTH_INTERNAL int apth_wait_event_list(struct list *el)
{
    if (list_empty(el))
        return apth_error(-1, EINVAL);

    apth_t self = CUR_APTH;

    apth_debug("apth_wait_events: enter from thread \"%s\"", self->name);

    // Mark all events in the list as still pending
    FOR_ELEMENT_IN_LIST_REF(el, e)
    {
        apth_event_t ev = apth_event_t_list_entry(e);
        ev->ev_status = APTH_EV_STATUS_PENDING;
        apth_debug("apth_wait_events: waiting on event 0x%lx", (unsigned long)ev);
    }

    // Link event list to current thread
    self->event_list = *el;

    // Eagerly register FD events before yielding.
    // With inline poller, this is cheap (just list_push_back + conditional
    // epoll_ctl ADD on first use). No reactor wake syscall needed.
    {
        apth_sched_t sched = SCHED_OF(self);
        FOR_ELEMENT_IN_LIST(self->event_list, pre_e)
        {
            apth_event_t pre_ev = apth_event_t_list_entry(pre_e);
            if (pre_ev->ev_type == APTH_EVENT_TYPE_FD)
            {
#ifdef APTH_USE_REACTOR
                apth_reactor_add_waiter(sched, pre_ev->ev_args.FD.fd, self, pre_ev);
#else
                epoll_map_add_waiter(sched, pre_ev->ev_args.FD.fd, self, pre_ev);
#endif
            }
        }
    }

    // Move apth into waiting state and transfer control to scheduler
    atomic_store_release(&self->state, APTH_STATE_WAITING);
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    // Check for cancellation
    apth_cancel_point();

    // Unlink events from current thread
    list_init(&self->event_list);

    // Count number of actually occurred (or failed) events
    int nonpending = 0;
    FOR_ELEMENT_IN_LIST_REF(el, e2)
    {
        apth_event_t ev = apth_event_t_list_entry(e2);
        if (ev->ev_status != APTH_EV_STATUS_PENDING)
        {
            apth_debug("apth_wait_event_list: non-pending event 0x%lx", (unsigned long)ev);
            nonpending++;
        }
    }

    apth_debug("apth_wait_event_list: leave to thread \"%s\"", self->name);
    return nonpending;
}

APTH_INTERNAL bool apth_wait_event(apth_event_t ev)
{
    if (ev == APTH_EVENT_NULL)
        return apth_error(false, EINVAL);
    apth_t self = CUR_APTH;
    assert(SCHED_OF(self) == CUR_SCHED);
    apth_debug("enter from thread \"%s\"", self->name);

    // Mark the event as still pending
    ev->ev_status = APTH_EV_STATUS_PENDING;

    // Link event into current thread
    apth_event_list_add(&self->event_list, ev);

    // Eagerly register FD event
    if (ev->ev_type == APTH_EVENT_TYPE_FD)
    {
#ifdef APTH_USE_REACTOR
        apth_reactor_add_waiter(SCHED_OF(self), ev->ev_args.FD.fd, self, ev);
#else
        epoll_map_add_waiter(SCHED_OF(self), ev->ev_args.FD.fd, self, ev);
#endif
    }

    // Move thread into waiting state and transfer control to scheduler
    atomic_store_release(&self->state, APTH_STATE_WAITING);
    self->yield_reason = APTH_YIELD_REASON_WAIT;
    apth_yield();

    // Check for cancellation
    apth_cancel_point();

    // Unlink event from current thread
    list_remove(&ev->elem);
    assert_msg(list_empty(&self->event_list), "insane");

    bool result = false;
    if (ev->ev_status != APTH_EV_STATUS_PENDING)
    {
        result = true;
    }

    apth_debug("leave to thread \"%s\"", self->name);
    return result;
}
