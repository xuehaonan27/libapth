#include "apth_reactor.h"
#include "hook_libc/hooked_funcs.h"
#include "internal/apth_sched.h"
#include "internal/apth_fd.h"
#include "utils/debug.h"
#include "utils/lll.inline.h"
#include "utils/list.inline.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>

/* The single global reactor instance. */
struct apth_reactor GLOBAL_REACTOR;

/* ================================================================
 * Waiter pool (reactor-owned, accessed only from reactor thread)
 * ================================================================ */

static struct apth_epoll_waiter *reactor_alloc_waiter(void)
{
    struct apth_reactor *r = &GLOBAL_REACTOR;
    if (!list_empty(&r->free_waiters))
    {
        struct list_elem *e = list_pop_front(&r->free_waiters);
        return apth_epoll_waiter_list_entry(e);
    }
    r->waiter_pool_allocated++;
    return (struct apth_epoll_waiter *)malloc(sizeof(struct apth_epoll_waiter));
}

static void reactor_free_waiter(struct apth_epoll_waiter *w)
{
    struct apth_reactor *r = &GLOBAL_REACTOR;
    if (w >= r->waiter_pool && w < r->waiter_pool + r->waiter_pool_size)
        list_push_back(&r->free_waiters, &w->elem);
    else
    {
        free(w);
        r->waiter_pool_allocated--;
    }
}

/* ================================================================
 * FD slot management (reactor-owned)
 * ================================================================ */

static void reactor_ensure_fd_slot_capacity(int fd)
{
    struct apth_reactor *r = &GLOBAL_REACTOR;
    if (fd < r->fd_slot_capacity)
        return;

    int new_cap = r->fd_slot_capacity;
    while (new_cap <= fd)
        new_cap *= 2;

    struct apth_epoll_fd_slot *new_slots =
        (struct apth_epoll_fd_slot *)calloc(new_cap, sizeof(struct apth_epoll_fd_slot));
    if (new_slots == NULL)
        return;

    memcpy(new_slots, r->fd_slots, r->fd_slot_capacity * sizeof(struct apth_epoll_fd_slot));

    /* Rebuild active list and fix waiter list pointers. */
    list_init(&r->active_fd_slots);
    r->active_fd_count = 0;
    for (int i = 0; i < r->fd_slot_capacity; i++)
    {
        if (new_slots[i].waiter_count > 0)
        {
            struct list *old_list = &r->fd_slots[i].waiters;
            struct list *new_list = &new_slots[i].waiters;
            if (!list_empty(old_list))
            {
                new_list->head.next = old_list->head.next;
                new_list->tail.prev = old_list->tail.prev;
                new_list->head.next->prev = &new_list->head;
                new_list->tail.prev->next = &new_list->tail;
            }
            else
                list_init(new_list);
        }
        if (new_slots[i].registered)
        {
            list_push_back(&r->active_fd_slots, &new_slots[i].elem);
            r->active_fd_count++;
        }
    }
    for (int i = r->fd_slot_capacity; i < new_cap; i++)
    {
        new_slots[i].fd = i;
        list_init(&new_slots[i].waiters);
        new_slots[i].waiter_count = 0;
        new_slots[i].registered = false;
    }

    free(r->fd_slots);
    r->fd_slots = new_slots;
    r->fd_slot_capacity = new_cap;
}

/* ================================================================
 * Process a single request (reactor thread only)
 * ================================================================ */

static void reactor_process_watch(struct reactor_request *req)
{
    struct apth_reactor *r = &GLOBAL_REACTOR;
    int fd = req->fd;
    if (fd < 0)
        return;

    reactor_ensure_fd_slot_capacity(fd);
    if (fd >= r->fd_slot_capacity)
        return;

    struct apth_epoll_fd_slot *slot = &r->fd_slots[fd];

    struct apth_epoll_waiter *w = reactor_alloc_waiter();
    if (!w)
        return;
    w->th = req->th;
    w->ev = req->ev;
    req->ev->epoll_waiter = w;

    list_push_back(&slot->waiters, &w->elem);
    slot->waiter_count++;

    if (!slot->registered)
    {
        struct epoll_event ee;
        ee.events = EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLET;
        ee.data.fd = fd;
        int rc = epoll_ctl(r->epoll_fd, EPOLL_CTL_ADD, fd, &ee);
        if (rc < 0 && errno != EEXIST)
        {
            list_remove(&w->elem);
            slot->waiter_count--;
            req->ev->epoll_waiter = NULL;
            reactor_free_waiter(w);
            __atomic_store_n(&req->ev->ev_status, APTH_EV_STATUS_FAILED, __ATOMIC_RELEASE);
            apth_sched_wake(req->sched);
            return;
        }
        slot->registered = true;
        list_push_back(&r->active_fd_slots, &slot->elem);
        r->active_fd_count++;
    }
}

static void reactor_process_unwatch(struct reactor_request *req)
{
    struct apth_reactor *r = &GLOBAL_REACTOR;
    int fd = req->fd;
    if (fd < 0 || fd >= r->fd_slot_capacity)
        return;

    struct apth_epoll_fd_slot *slot = &r->fd_slots[fd];
    struct apth_epoll_waiter *w = req->ev->epoll_waiter;
    if (w)
    {
        list_remove(&w->elem);
        reactor_free_waiter(w);
        req->ev->epoll_waiter = NULL;
        slot->waiter_count--;
    }
}

static void reactor_process_fd_close(struct reactor_request *req)
{
    struct apth_reactor *r = &GLOBAL_REACTOR;
    int fd = req->fd;
    if (fd < 0 || fd >= r->fd_slot_capacity)
        return;

    struct apth_epoll_fd_slot *slot = &r->fd_slots[fd];
    if (slot->waiter_count == 0)
    {
        /* Even with no waiters, clear the epoll registration so a new
         * FD with the same number can be registered cleanly. */
        if (slot->registered)
        {
            slot->registered = false;
            list_remove(&slot->elem);
            r->active_fd_count--;
        }
        return;
    }

    struct list_elem *e = list_begin(&slot->waiters);
    while (e != list_end(&slot->waiters))
    {
        struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
        struct list_elem *next = list_next(e);

        __atomic_store_n(&w->ev->ev_status, APTH_EV_STATUS_FAILED, __ATOMIC_RELEASE);
        w->ev->epoll_registered = false;
        w->ev->epoll_waiter = NULL;

        apth_sched_t ws = w->th->current_sched;
        if (ws)
            apth_sched_wake(ws);

        list_remove(&w->elem);
        reactor_free_waiter(w);
        e = next;
    }
    slot->waiter_count = 0;

    if (slot->registered)
    {
        /* Don't call epoll_ctl(DEL) — the FD is already closed by the caller,
         * and the kernel auto-removes closed FDs from epoll. */
        slot->registered = false;
        list_remove(&slot->elem);
        r->active_fd_count--;
    }
}

/* ================================================================
 * Drain the request queue (reactor thread)
 * ================================================================ */

static void reactor_drain_requests(void)
{
    struct apth_reactor *r = &GLOBAL_REACTOR;

    struct reactor_request local_reqs[REACTOR_REQ_QUEUE_SIZE];
    int local_count;

    lll_internal_lock(&r->req_lock);
    local_count = r->req_count;
    if (local_count > 0)
    {
        memcpy(local_reqs, r->reqs, local_count * sizeof(struct reactor_request));
        r->req_count = 0;
    }
    lll_internal_unlock(&r->req_lock);

    for (int i = 0; i < local_count; i++)
    {
        switch (local_reqs[i].type)
        {
        case REACTOR_REQ_WATCH:
            reactor_process_watch(&local_reqs[i]);
            break;
        case REACTOR_REQ_UNWATCH:
            reactor_process_unwatch(&local_reqs[i]);
            break;
        case REACTOR_REQ_FD_CLOSE:
            reactor_process_fd_close(&local_reqs[i]);
            break;
        }
    }
}

/* ================================================================
 * Process epoll completions (reactor thread)
 * ================================================================ */

static void reactor_process_epoll_events(struct epoll_event *events, int nready)
{
    struct apth_reactor *r = &GLOBAL_REACTOR;

    for (int i = 0; i < nready; i++)
    {
        if (events[i].data.fd == r->wake_fd)
        {
            uint64_t val;
            ssize_t ignored = apth_func_raw(read)(r->wake_fd, &val, sizeof(val));
            (void)ignored;
            continue;
        }

        int fd = events[i].data.fd;
        uint32_t revents = events[i].events;

        if (fd < 0 || fd >= r->fd_slot_capacity)
            continue;

        struct apth_epoll_fd_slot *slot = &r->fd_slots[fd];
        struct list_elem *e = list_begin(&slot->waiters);

        while (e != list_end(&slot->waiters))
        {
            struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
            struct list_elem *next = list_next(e);

            bool matched = false;
            apth_goal_t goal = w->ev->ev_goal;
            if ((goal & APTH_GOAL_UNTIL_FD_READABLE) && (revents & EPOLLIN))
                matched = true;
            if ((goal & APTH_GOAL_UNTIL_FD_WRITEABLE) && (revents & EPOLLOUT))
                matched = true;
            if ((goal & APTH_GOAL_UNTIL_FD_EXCEPTION) && (revents & EPOLLPRI))
                matched = true;
            if (revents & (EPOLLERR | EPOLLHUP))
                matched = true;

            if (matched)
            {
                __atomic_store_n(&w->ev->ev_status, APTH_EV_STATUS_OCCURRED, __ATOMIC_RELEASE);
                w->ev->epoll_waiter = NULL;
                w->ev->epoll_registered = false;

                apth_sched_t ws = w->th->current_sched;
                if (ws)
                    apth_sched_wake(ws);

                list_remove(&w->elem);
                slot->waiter_count--;
                reactor_free_waiter(w);
            }
            e = next;
        }
    }
}

/* ================================================================
 * Reactor main loop
 * ================================================================ */

static void *reactor_thread_func(void *arg)
{
    (void)arg;
    struct apth_reactor *r = &GLOBAL_REACTOR;

    apth_debug("reactor thread started");

    while (__atomic_load_n(&r->running, __ATOMIC_ACQUIRE))
    {
        reactor_drain_requests();

        struct epoll_event events[128];
        int nready = apth_func_raw(epoll_wait)(r->epoll_fd, events, 128, 5 /* ms */);

        if (nready > 0)
            reactor_process_epoll_events(events, nready);
    }

    apth_debug("reactor thread exiting");
    return NULL;
}

/* ================================================================
 * Public API: lifecycle
 * ================================================================ */

APTH_INTERNAL int apth_reactor_start(void)
{
    struct apth_reactor *r = &GLOBAL_REACTOR;
    memset(r, 0, sizeof(*r));

    r->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (r->epoll_fd < 0)
        return -1;

    r->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (r->wake_fd < 0)
    {
        close(r->epoll_fd);
        return -1;
    }

    struct epoll_event ee;
    ee.events = EPOLLIN | EPOLLET;
    ee.data.fd = r->wake_fd;
    if (epoll_ctl(r->epoll_fd, EPOLL_CTL_ADD, r->wake_fd, &ee) < 0)
    {
        close(r->wake_fd);
        close(r->epoll_fd);
        return -1;
    }

    lll_internal_init(&r->req_lock);
    r->req_count = 0;

    r->fd_slot_capacity = APTH_FD_TABLE_INIT_CAPACITY;
    r->fd_slots = (struct apth_epoll_fd_slot *)calloc(
        r->fd_slot_capacity, sizeof(struct apth_epoll_fd_slot));
    if (!r->fd_slots)
    {
        close(r->wake_fd);
        close(r->epoll_fd);
        return -1;
    }
    for (int i = 0; i < r->fd_slot_capacity; i++)
    {
        r->fd_slots[i].fd = i;
        list_init(&r->fd_slots[i].waiters);
        r->fd_slots[i].waiter_count = 0;
        r->fd_slots[i].registered = false;
    }
    list_init(&r->active_fd_slots);
    r->active_fd_count = 0;

    r->waiter_pool_size = REACTOR_WAITER_POOL_SIZE;
    r->waiter_pool = (struct apth_epoll_waiter *)calloc(
        r->waiter_pool_size, sizeof(struct apth_epoll_waiter));
    if (!r->waiter_pool)
    {
        free(r->fd_slots);
        close(r->wake_fd);
        close(r->epoll_fd);
        return -1;
    }
    list_init(&r->free_waiters);
    for (int i = 0; i < r->waiter_pool_size; i++)
        list_push_back(&r->free_waiters, &r->waiter_pool[i].elem);
    r->waiter_pool_allocated = 0;

    __atomic_store_n(&r->running, true, __ATOMIC_RELEASE);

    int ret = pthread_create(&r->thread, NULL, reactor_thread_func, NULL);
    if (ret != 0)
    {
        __atomic_store_n(&r->running, false, __ATOMIC_RELEASE);
        free(r->waiter_pool);
        free(r->fd_slots);
        close(r->wake_fd);
        close(r->epoll_fd);
        return -1;
    }

    apth_debug("reactor started (epoll_fd=%d, wake_fd=%d)", r->epoll_fd, r->wake_fd);
    return 0;
}

APTH_INTERNAL void apth_reactor_stop(void)
{
    struct apth_reactor *r = &GLOBAL_REACTOR;
    if (!__atomic_load_n(&r->running, __ATOMIC_ACQUIRE))
        return;

    __atomic_store_n(&r->running, false, __ATOMIC_RELEASE);

    uint64_t val = 1;
    ssize_t ignored = write(r->wake_fd, &val, sizeof(val));
    (void)ignored;

    pthread_join(r->thread, NULL);

    for (int i = 0; i < r->fd_slot_capacity; i++)
    {
        struct apth_epoll_fd_slot *slot = &r->fd_slots[i];
        while (!list_empty(&slot->waiters))
        {
            struct list_elem *e = list_pop_front(&slot->waiters);
            struct apth_epoll_waiter *w = apth_epoll_waiter_list_entry(e);
            if (w >= r->waiter_pool && w < r->waiter_pool + r->waiter_pool_size)
                ;
            else
                free(w);
        }
    }
    free(r->fd_slots);
    free(r->waiter_pool);
    close(r->wake_fd);
    close(r->epoll_fd);

    apth_debug("reactor stopped");
}

/* ================================================================
 * Public API: request submission (thread-safe)
 * ================================================================ */

static void reactor_wake(void)
{
    uint64_t val = 1;
    ssize_t ignored = apth_func_raw(write)(GLOBAL_REACTOR.wake_fd, &val, sizeof(val));
    (void)ignored;
}

APTH_INTERNAL int apth_reactor_submit_watch(int fd, apth_goal_t goal,
                                             apth_event_t ev, apth_t th,
                                             apth_sched_t sched)
{
    struct apth_reactor *r = &GLOBAL_REACTOR;

    lll_internal_lock(&r->req_lock);
    if (r->req_count >= REACTOR_REQ_QUEUE_SIZE)
    {
        lll_internal_unlock(&r->req_lock);
        return -1;
    }
    struct reactor_request *req = &r->reqs[r->req_count++];
    req->type = REACTOR_REQ_WATCH;
    req->fd = fd;
    req->goal = goal;
    req->ev = ev;
    req->th = th;
    req->sched = sched;
    lll_internal_unlock(&r->req_lock);

    ev->epoll_registered = true;
    reactor_wake();
    return 0;
}

APTH_INTERNAL void apth_reactor_submit_unwatch(int fd, apth_event_t ev)
{
    struct apth_reactor *r = &GLOBAL_REACTOR;

    /* UNWATCH is a cleanup operation and must not be silently dropped.
     * Spin-retry if the queue is full — the reactor drains it promptly. */
    for (;;)
    {
        lll_internal_lock(&r->req_lock);
        if (r->req_count < REACTOR_REQ_QUEUE_SIZE)
        {
            struct reactor_request *req = &r->reqs[r->req_count++];
            req->type = REACTOR_REQ_UNWATCH;
            req->fd = fd;
            req->ev = ev;
            req->th = NULL;
            req->sched = NULL;
            lll_internal_unlock(&r->req_lock);
            break;
        }
        lll_internal_unlock(&r->req_lock);
        reactor_wake(); /* Prod reactor to drain. */
        sched_yield();
    }

    ev->epoll_registered = false;
}

APTH_INTERNAL void apth_reactor_notify_fd_closed(int fd)
{
    struct apth_reactor *r = &GLOBAL_REACTOR;

    /* FD_CLOSE must not be dropped — stale epoll registrations cause
     * mis-woken threads if the FD number is reused by the kernel. */
    for (;;)
    {
        lll_internal_lock(&r->req_lock);
        if (r->req_count < REACTOR_REQ_QUEUE_SIZE)
        {
            struct reactor_request *req = &r->reqs[r->req_count++];
            req->type = REACTOR_REQ_FD_CLOSE;
            req->fd = fd;
            req->ev = NULL;
            req->th = NULL;
            req->sched = NULL;
            lll_internal_unlock(&r->req_lock);
            break;
        }
        lll_internal_unlock(&r->req_lock);
        reactor_wake(); /* Prod reactor to drain. */
        sched_yield();
    }

    reactor_wake();
}
