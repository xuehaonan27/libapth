#include "apth_reactor.h"
#include "internal/apth_fd.h"
#include "internal/apth_sched.h"
#include "internal/types.h"
#include "hook_libc/hooked_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/lll.inline.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#ifndef APTH_NUMA
struct apth_reactor APTH_GLOBAL_REACTOR;
#endif

#define REACTOR_WAITER_POOL_SIZE 512
#define REACTOR_EPOLL_MAX_EVENTS 64

// ==================== Waiter pool ====================

static struct apth_epoll_waiter *reactor_alloc_waiter(struct apth_reactor *r)
{
    if (!list_empty(&r->free_waiters))
    {
        struct list_elem *e = list_pop_front(&r->free_waiters);
        return apth_epoll_waiter_list_entry(e);
    }
    // Pool exhausted, fall back to malloc
    r->waiter_pool_allocated++;
    return (struct apth_epoll_waiter *)malloc(sizeof(struct apth_epoll_waiter));
}

static void reactor_free_waiter(struct apth_reactor *r, struct apth_epoll_waiter *w)
{
    if (w >= r->waiter_pool && w < r->waiter_pool + r->waiter_pool_size)
        list_push_back(&r->free_waiters, &w->elem);
    else
    {
        free(w);
        r->waiter_pool_allocated--;
    }
}

// ==================== Request pool ====================

static struct apth_reactor_request *reactor_alloc_req(struct apth_reactor *r)
{
    lll_internal_lock(&r->req_pool_lock);
    struct apth_reactor_request *req = NULL;
    if (!list_empty(&r->free_reqs))
    {
        struct list_elem *e = list_pop_front(&r->free_reqs);
        req = list_entry(e, struct apth_reactor_request, elem);
    }
    lll_internal_unlock(&r->req_pool_lock);

    if (req == NULL)
        req = (struct apth_reactor_request *)malloc(sizeof(struct apth_reactor_request));
    return req;
}

static void reactor_free_req(struct apth_reactor *r, struct apth_reactor_request *req)
{
    if (req >= r->req_pool && req < r->req_pool + APTH_REACTOR_REQ_POOL_SIZE)
    {
        lll_internal_lock(&r->req_pool_lock);
        list_push_back(&r->free_reqs, &req->elem);
        lll_internal_unlock(&r->req_pool_lock);
    }
    else
        free(req);
}

// ==================== FD slot operations (reactor thread only) ====================

static void reactor_ensure_fd_slot_capacity(struct apth_reactor *r, int fd)
{
    if (fd < r->fd_slot_capacity)
        return;

    int new_cap = r->fd_slot_capacity;
    while (new_cap <= fd)
        new_cap *= 2;

    struct apth_epoll_fd_slot *new_slots = (struct apth_epoll_fd_slot *)calloc(new_cap, sizeof(struct apth_epoll_fd_slot));
    if (new_slots == NULL)
        return; // Silently fail; caller will handle

    // Copy existing slots
    memcpy(new_slots, r->fd_slots, r->fd_slot_capacity * sizeof(struct apth_epoll_fd_slot));

    // Fix up list pointers: active_fd_slots and dirty_fd_slots contain elem/dirty_elem
    // from the old array. We need to re-link them from the new array.
    // Strategy: rebuild both lists by scanning all slots.
    list_init(&r->active_fd_slots);
    list_init(&r->dirty_fd_slots);
    r->active_fd_count = 0;

    for (int i = 0; i < r->fd_slot_capacity; i++)
    {
        // Preserve waiter lists by re-initializing the list head to point to new location
        if (new_slots[i].waiter_count > 0)
        {
            // The list elements within waiters point back to the old slot's list head.
            // We need to fixup the sentinel pointers.
            struct list *old_list = &r->fd_slots[i].waiters;
            struct list *new_list = &new_slots[i].waiters;
            // Re-link: the first and last elements point back to the old sentinel
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
            list_push_back(&r->active_fd_slots, &new_slots[i].elem);
            r->active_fd_count++;
        }
        if (new_slots[i].on_dirty_list)
        {
            list_push_back(&r->dirty_fd_slots, &new_slots[i].dirty_elem);
        }
    }

    // Initialize new slots
    for (int i = r->fd_slot_capacity; i < new_cap; i++)
    {
        new_slots[i].fd = i;
        new_slots[i].aggregate_events = 0;
        list_init(&new_slots[i].waiters);
        new_slots[i].waiter_count = 0;
        new_slots[i].registered = false;
        new_slots[i].readable_count = 0;
        new_slots[i].writeable_count = 0;
        new_slots[i].exception_count = 0;
        new_slots[i].epoll_dirty = false;
        new_slots[i].on_dirty_list = false;
    }

    free(r->fd_slots);
    r->fd_slots = new_slots;
    r->fd_slot_capacity = new_cap;
}

static int reactor_do_add_waiter(struct apth_reactor *r, int fd, apth_t th, apth_event_t ev)
{
    if (fd < 0)
        return -1;
    if (ev->epoll_registered)
        return 0;

    reactor_ensure_fd_slot_capacity(r, fd);
    if (fd >= r->fd_slot_capacity)
        return -1;

    struct apth_epoll_fd_slot *slot = &r->fd_slots[fd];

    struct apth_epoll_waiter *w = reactor_alloc_waiter(r);
    if (w == NULL)
        return -1;
    w->th = th;
    w->ev = ev;
    ev->epoll_waiter = w;

    uint32_t needed = 0;
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
    {
        needed |= EPOLLIN;
        slot->readable_count++;
    }
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
    {
        needed |= EPOLLOUT;
        slot->writeable_count++;
    }
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
    {
        needed |= EPOLLPRI;
        slot->exception_count++;
    }

    list_push_back(&slot->waiters, &w->elem);
    slot->waiter_count++;

    uint32_t old_aggregate = slot->aggregate_events;
    slot->aggregate_events = 0;
    if (slot->readable_count > 0)
        slot->aggregate_events |= EPOLLIN;
    if (slot->writeable_count > 0)
        slot->aggregate_events |= EPOLLOUT;
    if (slot->exception_count > 0)
        slot->aggregate_events |= EPOLLPRI;

    if (!slot->registered)
    {
        list_push_back(&r->active_fd_slots, &slot->elem);
        r->active_fd_count++;

        struct epoll_event ee;
        ee.events = slot->aggregate_events;
        ee.data.fd = fd;
        int rc = epoll_ctl(r->epoll_fd, EPOLL_CTL_ADD, fd, &ee);
        if (rc < 0)
        {
            list_remove(&w->elem);
            slot->waiter_count--;
            slot->aggregate_events = old_aggregate;
            if (slot->waiter_count == 0)
            {
                list_remove(&slot->elem);
                r->active_fd_count--;
            }
            ev->epoll_waiter = NULL;
            reactor_free_waiter(r, w);
            return -1;
        }
        slot->registered = true;
    }
    else if (slot->aggregate_events != old_aggregate)
    {
        slot->epoll_dirty = true;
        if (!slot->on_dirty_list)
        {
            list_push_back(&r->dirty_fd_slots, &slot->dirty_elem);
            slot->on_dirty_list = true;
        }
    }

    ev->epoll_registered = true;
    return 0;
}

static void reactor_do_remove_waiter(struct apth_reactor *r, int fd, apth_event_t ev)
{
    if (fd < 0 || fd >= r->fd_slot_capacity)
        return;
    if (!ev->epoll_registered)
        return;

    struct apth_epoll_fd_slot *slot = &r->fd_slots[fd];

    struct apth_epoll_waiter *w = ev->epoll_waiter;
    if (w != NULL)
    {
        if (ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
            slot->readable_count--;
        if (ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
            slot->writeable_count--;
        if (ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
            slot->exception_count--;

        list_remove(&w->elem);
        reactor_free_waiter(r, w);
        ev->epoll_waiter = NULL;
        slot->waiter_count--;
    }

    if (slot->waiter_count == 0)
    {
        if (slot->registered)
        {
            epoll_ctl(r->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
            slot->registered = false;
            list_remove(&slot->elem);
            r->active_fd_count--;
        }
        slot->aggregate_events = 0;
        slot->readable_count = 0;
        slot->writeable_count = 0;
        slot->exception_count = 0;
        if (slot->on_dirty_list)
        {
            list_remove(&slot->dirty_elem);
            slot->on_dirty_list = false;
            slot->epoll_dirty = false;
        }
    }
    else
    {
        uint32_t new_aggregate = 0;
        if (slot->readable_count > 0)
            new_aggregate |= EPOLLIN;
        if (slot->writeable_count > 0)
            new_aggregate |= EPOLLOUT;
        if (slot->exception_count > 0)
            new_aggregate |= EPOLLPRI;

        if (new_aggregate != slot->aggregate_events)
        {
            slot->aggregate_events = new_aggregate;
            slot->epoll_dirty = true;
            if (!slot->on_dirty_list)
            {
                list_push_back(&r->dirty_fd_slots, &slot->dirty_elem);
                slot->on_dirty_list = true;
            }
        }
    }

    ev->epoll_registered = false;
}

static void reactor_do_fd_closed(struct apth_reactor *r, int fd)
{
    if (fd < 0 || fd >= r->fd_slot_capacity)
        return;

    struct apth_epoll_fd_slot *slot = &r->fd_slots[fd];
    if (slot->waiter_count == 0)
        return;

    // Fail all waiters, collect unique threads to wake
#define REACTOR_CLOSE_WAKE_MAX 256
    apth_t wake_batch[REACTOR_CLOSE_WAKE_MAX];
    int wake_count = 0;

    struct list_elem *e = list_begin(&slot->waiters);
    while (e != list_end(&slot->waiters))
    {
        struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
        struct list_elem *next = list_next(e);

        w->ev->ev_status = APTH_EV_STATUS_FAILED;
        w->ev->epoll_registered = false;
        w->ev->epoll_waiter = NULL;

        // Collect unique threads
        if (!w->th->wake_pending && wake_count < REACTOR_CLOSE_WAKE_MAX)
        {
            w->th->wake_pending = true;
            wake_batch[wake_count++] = w->th;
        }

        list_remove(&w->elem);
        reactor_free_waiter(r, w);
        e = next;
    }

    slot->waiter_count = 0;
    slot->aggregate_events = 0;
    slot->readable_count = 0;
    slot->writeable_count = 0;
    slot->exception_count = 0;

    if (slot->registered)
    {
        epoll_ctl(r->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        slot->registered = false;
        list_remove(&slot->elem);
        r->active_fd_count--;
    }
    if (slot->on_dirty_list)
    {
        list_remove(&slot->dirty_elem);
        slot->on_dirty_list = false;
        slot->epoll_dirty = false;
    }

    // Wake affected schedulers
    for (int i = 0; i < wake_count; i++)
    {
        apth_t th = wake_batch[i];
        th->wake_pending = false;
        apth_sched_wake(SCHED_OF(th));
    }
}

static void reactor_flush_dirty(struct apth_reactor *r)
{
    struct list_elem *e = list_begin(&r->dirty_fd_slots);
    while (e != list_end(&r->dirty_fd_slots))
    {
        struct apth_epoll_fd_slot *slot = list_entry(e, struct apth_epoll_fd_slot, dirty_elem);
        struct list_elem *next = list_next(e);

        if (slot->registered)
        {
            struct epoll_event ee;
            ee.events = slot->aggregate_events;
            ee.data.fd = slot->fd;
            epoll_ctl(r->epoll_fd, EPOLL_CTL_MOD, slot->fd, &ee);
            slot->epoll_dirty = false;
        }

        list_remove(e);
        slot->on_dirty_list = false;
        e = next;
    }
}

static void reactor_wake_fd(struct apth_reactor *r, int fd, uint32_t revents,
                            apth_t *wake_batch, int *wake_count, int wake_max)
{
    if (fd < 0 || fd >= r->fd_slot_capacity)
        return;

    struct apth_epoll_fd_slot *slot = &r->fd_slots[fd];
    int waked = 0;

    struct list_elem *e = list_begin(&slot->waiters);
    while (e != list_end(&slot->waiters))
    {
        struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
        struct list_elem *next = list_next(e);

        bool matched = false;
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE) && (revents & EPOLLIN))
        {
            matched = true;
            slot->readable_count--;
        }
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE) && (revents & EPOLLOUT))
        {
            matched = true;
            slot->writeable_count--;
        }
        if ((w->ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION) && (revents & EPOLLPRI))
        {
            matched = true;
            slot->exception_count--;
        }

        if (revents & (EPOLLERR | EPOLLHUP))
        {
            if (!matched)
            {
                if (w->ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)
                    slot->readable_count--;
                if (w->ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)
                    slot->writeable_count--;
                if (w->ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)
                    slot->exception_count--;
            }
            matched = true;
        }

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
            reactor_free_waiter(r, w);
            waked++;
        }

        e = next;
    }

    if (slot->waiter_count == 0 && slot->registered)
    {
        epoll_ctl(r->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        slot->registered = false;
        list_remove(&slot->elem);
        r->active_fd_count--;
        slot->aggregate_events = 0;
        slot->readable_count = 0;
        slot->writeable_count = 0;
        slot->exception_count = 0;
        if (slot->on_dirty_list)
        {
            list_remove(&slot->dirty_elem);
            slot->on_dirty_list = false;
            slot->epoll_dirty = false;
        }
    }
    else if (waked > 0)
    {
        uint32_t new_aggregate = 0;
        if (slot->readable_count > 0)
            new_aggregate |= EPOLLIN;
        if (slot->writeable_count > 0)
            new_aggregate |= EPOLLOUT;
        if (slot->exception_count > 0)
            new_aggregate |= EPOLLPRI;

        if (new_aggregate != slot->aggregate_events)
        {
            slot->aggregate_events = new_aggregate;
            slot->epoll_dirty = true;
            if (!slot->on_dirty_list)
            {
                list_push_back(&r->dirty_fd_slots, &slot->dirty_elem);
                slot->on_dirty_list = true;
            }
        }
    }
}

// ==================== Reactor thread main loop ====================

static void *reactor_thread_main(void *arg)
{
    struct apth_reactor *r = (struct apth_reactor *)arg;

#define MAX_WAKE_BATCH 512
    apth_t wake_batch[MAX_WAKE_BATCH];
    int wake_count = 0;

    while (atomic_load_acquire(&r->running))
    {
        // 1. Drain request queue under lock
        struct list local_reqs;
        list_init(&local_reqs);

        lll_internal_lock(&r->req_lock);
        if (!list_empty(&r->req_queue))
        {
            // Move all requests to local list
            local_reqs.head.next = r->req_queue.head.next;
            local_reqs.tail.prev = r->req_queue.tail.prev;
            local_reqs.head.next->prev = &local_reqs.head;
            local_reqs.tail.prev->next = &local_reqs.tail;
            list_init(&r->req_queue);
        }
        lll_internal_unlock(&r->req_lock);

        // 2. Process each request
        while (!list_empty(&local_reqs))
        {
            struct list_elem *e = list_pop_front(&local_reqs);
            struct apth_reactor_request *req = list_entry(e, struct apth_reactor_request, elem);

            switch (req->type)
            {
            case REACTOR_REQ_ADD_WAITER:
                if (reactor_do_add_waiter(r, req->fd, req->th, req->ev) < 0)
                {
                    req->ev->ev_status = APTH_EV_STATUS_FAILED;
                    // Wake the scheduler so it notices the failure
                    if (req->sched)
                        apth_sched_wake(req->sched);
                }
                break;

            case REACTOR_REQ_REMOVE_WAITER:
                reactor_do_remove_waiter(r, req->fd, req->ev);
                break;

            case REACTOR_REQ_FD_CLOSED:
                reactor_do_fd_closed(r, req->fd);
                break;
            }

            reactor_free_req(r, req);
        }

        // 3. Process pending fd closes
        if (atomic_load_acquire(&r->pending_fd_close_count) > 0)
        {
            int local_fds[APTH_REACTOR_PENDING_FD_CLOSE_MAX];
            int local_count;

            lll_internal_lock(&r->pending_fd_close_lock);
            local_count = atomic_load_acquire(&r->pending_fd_close_count);
            if (local_count > APTH_REACTOR_PENDING_FD_CLOSE_MAX)
                local_count = APTH_REACTOR_PENDING_FD_CLOSE_MAX;
            memcpy(local_fds, r->pending_fd_close_fds, local_count * sizeof(int));
            atomic_store_release(&r->pending_fd_close_count, 0);
            lll_internal_unlock(&r->pending_fd_close_lock);

            for (int i = 0; i < local_count; i++)
                reactor_do_fd_closed(r, local_fds[i]);
        }

        // 4. Flush dirty epoll slots
        reactor_flush_dirty(r);

        // 5. epoll_wait
        struct epoll_event ep_events[REACTOR_EPOLL_MAX_EVENTS];
        int timeout_ms = (r->active_fd_count > 0) ? 10 : 50;
        int nready = epoll_wait(r->epoll_fd, ep_events, REACTOR_EPOLL_MAX_EVENTS, timeout_ms);

        wake_count = 0;

        if (nready > 0)
        {
            for (int i = 0; i < nready; i++)
            {
                int ready_fd = ep_events[i].data.fd;
                uint32_t revents = ep_events[i].events;

                // Drain the wake eventfd
                if (ready_fd == r->wake_eventfd)
                {
                    uint64_t val;
                    ssize_t __ignored = apth_func_raw(read)(r->wake_eventfd, &val, sizeof(val));
                    (void)__ignored;
                    continue;
                }

                reactor_wake_fd(r, ready_fd, revents,
                                wake_batch, &wake_count, MAX_WAKE_BATCH);
            }

            // For each woken thread: remove remaining FD events from reactor,
            // then wake the scheduler so it can transfer the thread from
            // waiting→waked in Phase 1 of the event manager.
            // We do NOT directly push to waked_queue because the thread
            // is still in the scheduler's waiting_queue list.
            for (int i = 0; i < wake_count; i++)
            {
                apth_t th = wake_batch[i];
                th->wake_pending = false;

                // Remove remaining pending FD events from reactor
                FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
                {
                    apth_event_t event = apth_event_t_list_entry(ev_e);
                    if (event->ev_type == APTH_EVENT_TYPE_FD &&
                        event->ev_status == APTH_EV_STATUS_PENDING)
                        reactor_do_remove_waiter(r, event->ev_args.FD.fd, event);
                }

                // Wake the target scheduler so it re-scans its waiting queue
                apth_sched_wake(SCHED_OF(th));
            }

            reactor_flush_dirty(r);
        }
    }

    return NULL;
#undef MAX_WAKE_BATCH
}

// ==================== Wake reactor ====================

static void reactor_wake(struct apth_reactor *r)
{
    uint64_t val = 1;
    ssize_t __ignored = apth_func_raw(write)(r->wake_eventfd, &val, sizeof(val));
    (void)__ignored;
}

// ==================== Public API ====================

APTH_INTERNAL int apth_reactor_init(void)
{
    struct apth_reactor *r = &APTH_GLOBAL_REACTOR;
    memset(r, 0, sizeof(*r));

    r->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (r->epoll_fd < 0)
        return -1;

    r->wake_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (r->wake_eventfd < 0)
    {
        apth_func_raw(close)(r->epoll_fd);
        return -1;
    }

    // Register wake_eventfd with epoll
    {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = r->wake_eventfd;
        if (epoll_ctl(r->epoll_fd, EPOLL_CTL_ADD, r->wake_eventfd, &ev) < 0)
        {
            apth_func_raw(close)(r->wake_eventfd);
            apth_func_raw(close)(r->epoll_fd);
            return -1;
        }
    }

    // Initialize fd slot table
    r->fd_slot_capacity = APTH_FD_TABLE_INIT_CAPACITY;
    r->fd_slots = (struct apth_epoll_fd_slot *)calloc(r->fd_slot_capacity, sizeof(struct apth_epoll_fd_slot));
    if (r->fd_slots == NULL)
    {
        apth_func_raw(close)(r->wake_eventfd);
        apth_func_raw(close)(r->epoll_fd);
        return -1;
    }
    for (int i = 0; i < r->fd_slot_capacity; i++)
    {
        r->fd_slots[i].fd = i;
        list_init(&r->fd_slots[i].waiters);
    }
    list_init(&r->active_fd_slots);
    list_init(&r->dirty_fd_slots);
    r->active_fd_count = 0;

    // Initialize waiter pool
    r->waiter_pool_size = REACTOR_WAITER_POOL_SIZE;
    r->waiter_pool = (struct apth_epoll_waiter *)calloc(r->waiter_pool_size, sizeof(struct apth_epoll_waiter));
    if (r->waiter_pool == NULL)
    {
        free(r->fd_slots);
        apth_func_raw(close)(r->wake_eventfd);
        apth_func_raw(close)(r->epoll_fd);
        return -1;
    }
    list_init(&r->free_waiters);
    for (int i = 0; i < r->waiter_pool_size; i++)
        list_push_back(&r->free_waiters, &r->waiter_pool[i].elem);
    r->waiter_pool_allocated = 0;

    // Initialize request queue
    lll_internal_init(&r->req_lock);
    list_init(&r->req_queue);

    // Initialize request pool
    lll_internal_init(&r->req_pool_lock);
    list_init(&r->free_reqs);
    for (int i = 0; i < APTH_REACTOR_REQ_POOL_SIZE; i++)
        list_push_back(&r->free_reqs, &r->req_pool[i].elem);

    // Initialize pending fd close
    atomic_store_release(&r->pending_fd_close_count, 0);
    lll_internal_init(&r->pending_fd_close_lock);

    // Start reactor thread
    atomic_store_release(&r->running, true);
    if (apth_func_raw(pthread_create)(&r->thread, NULL, reactor_thread_main, r) != 0)
    {
        atomic_store_release(&r->running, false);
        free(r->waiter_pool);
        free(r->fd_slots);
        apth_func_raw(close)(r->wake_eventfd);
        apth_func_raw(close)(r->epoll_fd);
        return -1;
    }

    // Detach name for debugging
    pthread_setname_np(r->thread, "apth-reactor");

    return 0;
}

APTH_INTERNAL void apth_reactor_destroy(void)
{
    struct apth_reactor *r = &APTH_GLOBAL_REACTOR;

    // Signal thread to stop
    atomic_store_release(&r->running, false);
    reactor_wake(r);
    apth_func_raw(pthread_join)(r->thread, NULL);

    // Clean up remaining waiters
    for (int i = 0; i < r->fd_slot_capacity; i++)
    {
        struct apth_epoll_fd_slot *slot = &r->fd_slots[i];
        while (!list_empty(&slot->waiters))
        {
            struct list_elem *e = list_pop_front(&slot->waiters);
            struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
            reactor_free_waiter(r, w);
        }
    }

    // Drain request queue
    while (!list_empty(&r->req_queue))
    {
        struct list_elem *e = list_pop_front(&r->req_queue);
        struct apth_reactor_request *req = list_entry(e, struct apth_reactor_request, elem);
        reactor_free_req(r, req);
    }

    free(r->fd_slots);
    r->fd_slots = NULL;
    free(r->waiter_pool);
    r->waiter_pool = NULL;

    apth_func_raw(close)(r->wake_eventfd);
    apth_func_raw(close)(r->epoll_fd);
    r->wake_eventfd = -1;
    r->epoll_fd = -1;
}

APTH_INTERNAL int apth_reactor_add_waiter(apth_sched_t sched, int fd, apth_t th, apth_event_t ev)
{
    struct apth_reactor *r = REACTOR_FOR(sched);

    struct apth_reactor_request *req = reactor_alloc_req(r);
    if (req == NULL)
        return -1;

    req->type = REACTOR_REQ_ADD_WAITER;
    req->fd = fd;
    req->th = th;
    req->ev = ev;
    req->sched = sched;

    lll_internal_lock(&r->req_lock);
    list_push_back(&r->req_queue, &req->elem);
    lll_internal_unlock(&r->req_lock);

    reactor_wake(r);
    return 0;
}

APTH_INTERNAL void apth_reactor_remove_waiter(apth_sched_t sched, int fd, apth_t th __attribute__((unused)), apth_event_t ev)
{
    struct apth_reactor *r = REACTOR_FOR(sched);

    struct apth_reactor_request *req = reactor_alloc_req(r);
    if (req == NULL)
        return;

    req->type = REACTOR_REQ_REMOVE_WAITER;
    req->fd = fd;
    req->th = th;
    req->ev = ev;
    req->sched = sched;

    lll_internal_lock(&r->req_lock);
    list_push_back(&r->req_queue, &req->elem);
    lll_internal_unlock(&r->req_lock);

    reactor_wake(r);
}

APTH_INTERNAL void apth_reactor_notify_fd_closed(int fd)
{
    struct apth_reactor *r = &APTH_GLOBAL_REACTOR;

    lll_internal_lock(&r->pending_fd_close_lock);
    int idx = atomic_load_acquire(&r->pending_fd_close_count);
    if (idx < APTH_REACTOR_PENDING_FD_CLOSE_MAX)
    {
        r->pending_fd_close_fds[idx] = fd;
        atomic_store_release(&r->pending_fd_close_count, idx + 1);
    }
    lll_internal_unlock(&r->pending_fd_close_lock);

    reactor_wake(r);
}
