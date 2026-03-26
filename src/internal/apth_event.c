#include "apth_event.h"
#include "hook_libc/hooked_funcs.h"
#include "internal/apth_cancel.h"
#include "internal/apth_fd.h"
#include "internal/apth_fd_slot.h"
#include "internal/apth_epoll_waiter.h"
#include "internal/apth_iouring.h"
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
#include <poll.h>

APTH_INTERNAL bool apth_state_matches_event_goal(apth_state_t state, apth_goal_t goal)
{
    return ((int)state == (int)goal);
}

// Forward declarations for io_uring backend (used before definition)
#ifdef APTH_USE_IOURING
static void uring_map_fail_all_waiters_for_fd(apth_sched_t sched, int fd);
#endif

// ==================== Per-scheduler inline poller ====================

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
    {
#ifdef APTH_USE_IOURING
        if (sched->use_iouring)
            uring_map_fail_all_waiters_for_fd(sched, local_fds[i]);
        else
#endif
            epoll_map_fail_all_waiters_for_fd(sched, local_fds[i]);
    }
}

// ==================== io_uring backend ====================
#ifdef APTH_USE_IOURING

// Re-arm the wake_eventfd poll (io_uring poll is one-shot)
static void uring_rearm_wake(apth_sched_t sched)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&sched->uring_ctx.ring);
    if (sqe)
    {
        io_uring_prep_poll_add(sqe, sched->wake_eventfd, POLLIN);
        io_uring_sqe_set_data64(sqe, URING_UD_WAKE);
    }
}

// Add a waiter using io_uring POLL_ADD (one-shot per waiter)
static int uring_map_add_waiter(apth_sched_t sched, int fd, apth_t th, apth_event_t ev)
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
    w->fd = fd;
    w->cancelled = false;
    ev->epoll_waiter = w;

    list_push_back(&slot->waiters, &w->elem);
    slot->waiter_count++;

    if (!slot->registered)
    {
        list_push_back(&sched->active_fd_slots, &slot->elem);
        sched->active_fd_count++;
        slot->registered = true;
    }

    // Convert event goal to poll mask
    uint32_t poll_mask = 0;
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_READABLE)  poll_mask |= POLLIN;
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_WRITEABLE)  poll_mask |= POLLOUT;
    if (ev->ev_goal & APTH_GOAL_UNTIL_FD_EXCEPTION)  poll_mask |= POLLPRI;

    // Submit one-shot POLL_ADD with waiter pointer as user_data
    struct io_uring_sqe *sqe = io_uring_get_sqe(&sched->uring_ctx.ring);
    if (!sqe)
    {
        // SQ full — flush and retry
        io_uring_submit(&sched->uring_ctx.ring);
        sqe = io_uring_get_sqe(&sched->uring_ctx.ring);
    }
    if (!sqe)
    {
        // Still full — fail
        list_remove(&w->elem);
        slot->waiter_count--;
        ev->epoll_waiter = NULL;
        free_waiter(sched, w);
        return -1;
    }

    io_uring_prep_poll_add(sqe, fd, poll_mask);
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)w);
    // Don't submit yet — batch at Phase 2

    ev->epoll_registered = true;
    return 0;
}

// Remove a waiter: mark cancelled, move to pending list.
// The CQE (either normal result or -ECANCELED) will free the waiter.
static void uring_map_remove_waiter(apth_sched_t sched, int fd,
                                     apth_t th __attribute__((unused)),
                                     apth_event_t ev)
{
    if (fd < 0 || fd >= sched->fd_slot_capacity) return;
    if (!ev->epoll_registered) return;

    struct apth_epoll_fd_slot *slot = &sched->fd_slots[fd];
    struct apth_epoll_waiter *w = ev->epoll_waiter;
    if (w)
    {
        // Remove from fd_slot, mark cancelled, move to pending list
        list_remove(&w->elem);
        slot->waiter_count--;
        w->cancelled = true;
        list_push_back(&sched->uring_pending_cancels, &w->elem);

        // Clear back-pointers before the CQE arrives (prevents use-after-free
        // if the event is stack-allocated and the thread resumes before the CQE)
        w->ev = NULL;
        w->th = NULL;

        // Submit cancel SQE (best-effort)
        struct io_uring_sqe *sqe = io_uring_get_sqe(&sched->uring_ctx.ring);
        if (sqe)
        {
            io_uring_prep_cancel64(sqe, (uint64_t)(uintptr_t)w, 0);
            io_uring_sqe_set_data64(sqe, URING_UD_IGNORE);
        }

        ev->epoll_waiter = NULL;
    }
    ev->epoll_registered = false;
}

// Fail all waiters for a closed FD (io_uring path)
static void uring_map_fail_all_waiters_for_fd(apth_sched_t sched, int fd)
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

        // Move to pending cancels — CQE (error or cancel) will free
        list_remove(&w->elem);
        w->cancelled = true;
        list_push_back(&sched->uring_pending_cancels, &w->elem);

        e = next;
    }
    slot->waiter_count = 0;
    // Note: slot->registered is kept for tracking; FD is gone anyway
}

// Process a single CQE: wake waiter or free cancelled waiter
static void uring_process_cqe(apth_sched_t sched, struct io_uring_cqe *cqe,
                               apth_t *wake_batch, int *wake_count, int wake_max)
{
    uint64_t ud = io_uring_cqe_get_data64(cqe);

    if (ud == URING_UD_IGNORE)
        return; // Cancel CQE, ignore

    if (ud == URING_UD_WAKE)
    {
        // Wake eventfd fired — drain and re-arm
        uint64_t val;
        ssize_t __ignored = apth_func_raw(read)(sched->wake_eventfd, &val, sizeof(val));
        (void)__ignored;
        uring_rearm_wake(sched);
        return;
    }

    // FD event: ud is waiter pointer
    struct apth_epoll_waiter *w = (struct apth_epoll_waiter *)(uintptr_t)ud;

    if (w->cancelled)
    {
        // Cancelled waiter — remove from pending_cancels and free
        list_remove(&w->elem);
        free_waiter(sched, w);
        return;
    }

    // Normal completion: mark event as occurred
    if (w->direct_io)
        w->ev->uring_io_result = cqe->res; // Store I/O result for caller

    w->ev->ev_status = APTH_EV_STATUS_OCCURRED;
    w->ev->epoll_waiter = NULL;
    w->ev->epoll_registered = false;

    if (!w->th->wake_pending && *wake_count < wake_max)
    {
        w->th->wake_pending = true;
        wake_batch[(*wake_count)++] = w->th;
    }

    // Remove from fd_slot and free
    if (w->fd >= 0 && w->fd < sched->fd_slot_capacity)
    {
        struct apth_epoll_fd_slot *slot = &sched->fd_slots[w->fd];
        list_remove(&w->elem);
        slot->waiter_count--;
    }
    free_waiter(sched, w);
}

// ==================== Direct I/O helpers ====================

// Get an SQE from the ring, flushing if full.
static struct io_uring_sqe *uring_get_sqe(apth_sched_t sched)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&sched->uring_ctx.ring);
    if (!sqe)
    {
        io_uring_submit(&sched->uring_ctx.ring);
        sqe = io_uring_get_sqe(&sched->uring_ctx.ring);
    }
    return sqe;
}

// Generic helper: submit a pre-prepared io_uring SQE and wait for completion.
// The caller has already called io_uring_prep_* on the SQE.
// Returns the CQE result (bytes transferred, new fd, etc.) or -1 with errno.
static ssize_t uring_direct_submit_and_wait(apth_sched_t sched, int fd,
                                             struct io_uring_sqe *sqe)
{
    struct apth_epoll_waiter *w = alloc_waiter(sched);
    if (!w) { errno = ENOMEM; return -1; }

    w->th = CUR_APTH;
    w->fd = fd;
    w->cancelled = false;
    w->direct_io = true;

    // Set up event on stack — reuse FD type so the event manager's Phase 1
    // and post-processing handle it correctly (skip re-registration since
    // epoll_registered is already true).
    struct apth_event_st ev;
    memset(&ev, 0, sizeof(ev));
    ev.ev_type = APTH_EVENT_TYPE_FD;
    ev.ev_status = APTH_EV_STATUS_PENDING;
    ev.ev_goal = APTH_GOAL_UNTIL_FD_READABLE;
    ev.ev_args.FD.fd = fd;
    ev.epoll_registered = true;   // Prevent eager re-registration
    ev.epoll_waiter = w;
    ev.uring_io_result = 0;

    w->ev = &ev;

    // Register with fd_slot for tracking / fd-close notification
    sched_ensure_fd_slot_capacity(sched, fd);
    if (fd >= sched->fd_slot_capacity)
    {
        free_waiter(sched, w);
        errno = ENOMEM;
        return -1;
    }
    struct apth_epoll_fd_slot *slot = &sched->fd_slots[fd];
    list_push_back(&slot->waiters, &w->elem);
    slot->waiter_count++;
    if (!slot->registered)
    {
        list_push_back(&sched->active_fd_slots, &slot->elem);
        sched->active_fd_count++;
        slot->registered = true;
    }

    // Set user_data so uring_process_cqe can find this waiter
    io_uring_sqe_set_data64(sqe, (uint64_t)(uintptr_t)w);

    // Yield to scheduler. The SQE is submitted in Phase 2's io_uring_submit().
    // Kernel FAST_POLL handles the poll+retry internally.
    apth_wait_event(&ev);

    ssize_t result = ev.uring_io_result;
    if (result < 0)
    {
        errno = (int)(-result);
        return -1;
    }
    return result;
}

// ---- Public direct I/O wrappers ----

APTH_INTERNAL ssize_t apth_uring_direct_read(int fd, void *buf, size_t count)
{
    apth_sched_t sched = CUR_SCHED;
    struct io_uring_sqe *sqe = uring_get_sqe(sched);
    if (!sqe) { errno = ENOMEM; return -1; }
    io_uring_prep_read(sqe, fd, buf, count, (uint64_t)-1);
    return uring_direct_submit_and_wait(sched, fd, sqe);
}

APTH_INTERNAL ssize_t apth_uring_direct_write(int fd, const void *buf, size_t count)
{
    apth_sched_t sched = CUR_SCHED;
    struct io_uring_sqe *sqe = uring_get_sqe(sched);
    if (!sqe) { errno = ENOMEM; return -1; }
    io_uring_prep_write(sqe, fd, buf, count, (uint64_t)-1);
    return uring_direct_submit_and_wait(sched, fd, sqe);
}

APTH_INTERNAL ssize_t apth_uring_direct_recv(int fd, void *buf, size_t len, int flags)
{
    apth_sched_t sched = CUR_SCHED;
    struct io_uring_sqe *sqe = uring_get_sqe(sched);
    if (!sqe) { errno = ENOMEM; return -1; }
    io_uring_prep_recv(sqe, fd, buf, len, flags);
    return uring_direct_submit_and_wait(sched, fd, sqe);
}

APTH_INTERNAL ssize_t apth_uring_direct_send(int fd, const void *buf, size_t len, int flags)
{
    apth_sched_t sched = CUR_SCHED;
    struct io_uring_sqe *sqe = uring_get_sqe(sched);
    if (!sqe) { errno = ENOMEM; return -1; }
    io_uring_prep_send(sqe, fd, buf, len, flags);
    return uring_direct_submit_and_wait(sched, fd, sqe);
}

APTH_INTERNAL int apth_uring_direct_accept(int fd, struct sockaddr *addr,
                                            socklen_t *addrlen, int flags)
{
    apth_sched_t sched = CUR_SCHED;
    struct io_uring_sqe *sqe = uring_get_sqe(sched);
    if (!sqe) { errno = ENOMEM; return -1; }
    io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
    return (int)uring_direct_submit_and_wait(sched, fd, sqe);
}

#endif // APTH_USE_IOURING

// ==================== Unified dispatch ====================
// These inline functions select the right backend at runtime.

static inline int fd_map_add_waiter(apth_sched_t sched, int fd, apth_t th, apth_event_t ev)
{
#ifdef APTH_USE_IOURING
    if (sched->use_iouring)
        return uring_map_add_waiter(sched, fd, th, ev);
#endif
    return epoll_map_add_waiter(sched, fd, th, ev);
}

static inline void fd_map_remove_waiter(apth_sched_t sched, int fd, apth_t th, apth_event_t ev)
{
#ifdef APTH_USE_IOURING
    if (sched->use_iouring)
    {
        uring_map_remove_waiter(sched, fd, th, ev);
        return;
    }
#endif
    epoll_map_remove_waiter(sched, fd, th, ev);
}

// ==================== Event manager ====================

APTH_INTERNAL void apth_sched_eventmanager_epoll(apth_sched_t sched, apth_time_t *now, bool dopoll)
{
    apth_debug("enter in %s mode", dopoll ? "polling" : "waiting");

    for (;;)
    {
        bool loop_repeat = false;

        // Phase 0: process pending fd close notifications
        sched_process_pending_fd_closes(sched);

        // ==================== Phase 1: traverse waiting queue ====================
        // Check non-FD events (timer, signal, TID, FUNC, SELECT, cancellation).
        // FD events are handled directly by epoll_wait in Phase 2.
        // Skip this phase entirely if the waiting queue is empty.

        apth_time_t nexttimer_value;
        apth_time_set(&nexttimer_value, APTH_TIME_ZERO);
        apth_event_t nexttimer_ev = APTH_EVENT_NULL;
        apth_t nexttimer_th = APTH_NULL;
        bool has_timer = false;
        size_t notified_ths = 0;

#define MAX_WAKE_BATCH 512
        apth_t wake_batch[MAX_WAKE_BATCH];
        int wake_count = 0;

        // Deferred SELECT events: collected under lock, executed after unlock.
        // This avoids calling the select() syscall while holding the waiting
        // queue lock, which previously blocked all other waiting-queue
        // operations for O(N_select_events) syscalls.
#define MAX_DEFERRED_SELECT 32
        struct {
            apth_event_t event;
            apth_t th;
        } deferred_selects[MAX_DEFERRED_SELECT];
        int deferred_select_count = 0;

        if (thqueue_size(THQUEUE(sched, waiting)) == 0)
            goto phase2;

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
                        if (fd_map_add_waiter(sched, event->ev_args.FD.fd, th, event) < 0)
                        {
                            event->ev_status = APTH_EV_STATUS_FAILED;
                            any_occurred = true;
                        }
                    }
                    break;

                case APTH_EVENT_TYPE_SELECT:
                    // Defer select() syscall until after we release the lock.
                    // The thread is in WAITING state so its stack (where the
                    // event struct lives) is frozen and safe to access later.
                    if (deferred_select_count < MAX_DEFERRED_SELECT)
                    {
                        deferred_selects[deferred_select_count].event = event;
                        deferred_selects[deferred_select_count].th = th;
                        deferred_select_count++;
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

        // Transfer waked threads directly to ready queue (skip waked queue).
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
                        fd_map_remove_waiter(sched, event->ev_args.FD.fd, th, event);
                    }
                }
                atomic_store_release(&th->state, APTH_STATE_READY);
                transfer_th(th, THQUEUE(sched, waiting), THQUEUE(sched, ready));
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

        // ==================== Deferred SELECT processing ====================
        // Execute select() syscalls OUTSIDE the waiting queue lock.
        // Threads are in WAITING state so their stacks are frozen.
        for (int si = 0; si < deferred_select_count; si++)
        {
            apth_event_t event = deferred_selects[si].event;
            apth_t th = deferred_selects[si].th;

            // Skip if already resolved by another event
            if (event->ev_status != APTH_EV_STATUS_PENDING)
                continue;

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
            while ((rc = apth_func_raw(select)(event->ev_args.SELECT.nfd,
                                                prfds, pwfds, pefds, &zero_tv)) < 0
                   && errno == EINTR)
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

                // Wake this thread
                FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
                {
                    apth_event_t ev2 = apth_event_t_list_entry(ev_e);
                    if (ev2->ev_type == APTH_EVENT_TYPE_FD && ev2->epoll_registered)
                        fd_map_remove_waiter(sched, ev2->ev_args.FD.fd, th, ev2);
                }
                atomic_store_release(&th->state, APTH_STATE_READY);
                transfer_th(th, THQUEUE(sched, waiting), THQUEUE(sched, ready));
                notified_ths++;
                dopoll = true;
            }
            else if (rc < 0)
            {
                event->ev_status = APTH_EV_STATUS_FAILED;
                // Wake on error too
                FOR_ELEMENT_IN_LIST(th->event_list, ev_e)
                {
                    apth_event_t ev2 = apth_event_t_list_entry(ev_e);
                    if (ev2->ev_type == APTH_EVENT_TYPE_FD && ev2->epoll_registered)
                        fd_map_remove_waiter(sched, ev2->ev_args.FD.fd, th, ev2);
                }
                atomic_store_release(&th->state, APTH_STATE_READY);
                transfer_th(th, THQUEUE(sched, waiting), THQUEUE(sched, ready));
                notified_ths++;
                dopoll = true;
            }
        }

    phase2:
        // ==================== Phase 2: I/O event polling ====================

        int timeout_ms;
        if (dopoll)
        {
            // We have ready work, but still do a quick non-blocking poll
            // to avoid starving I/O-waiting threads.
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
            // No timer, no ready work: block briefly.
            timeout_ms = 10;
        }

        if (timeout_ms >= 0)
        {
            apth_t fd_wake_batch[MAX_WAKE_BATCH];
            int fd_wake_count = 0;
            bool timed_out = false;

#ifdef APTH_USE_IOURING
            if (sched->use_iouring)
            {
                // ---- io_uring path ----
                struct io_uring *ring = &sched->uring_ctx.ring;

                if (dopoll || timeout_ms == 0)
                {
                    // Non-blocking: submit pending SQEs, peek CQEs
                    io_uring_submit(ring);

                    struct io_uring_cqe *cqe;
                    unsigned head;
                    int nr = 0;
                    io_uring_for_each_cqe(ring, head, cqe)
                    {
                        uring_process_cqe(sched, cqe,
                                          fd_wake_batch, &fd_wake_count, MAX_WAKE_BATCH);
                        nr++;
                    }
                    io_uring_cq_advance(ring, nr);

                    if (nr == 0 && !dopoll && has_timer)
                        timed_out = true;
                }
                else
                {
                    // Blocking: submit + wait with timeout
                    struct __kernel_timespec ts;
                    ts.tv_sec = timeout_ms / 1000;
                    ts.tv_nsec = (long long)(timeout_ms % 1000) * 1000000LL;

                    struct io_uring_cqe *cqe;
                    int ret = io_uring_submit_and_wait_timeout(ring, &cqe, 1, &ts, NULL);

                    if (ret >= 0)
                    {
                        // Process all available CQEs
                        unsigned head;
                        int nr = 0;
                        io_uring_for_each_cqe(ring, head, cqe)
                        {
                            uring_process_cqe(sched, cqe,
                                              fd_wake_batch, &fd_wake_count, MAX_WAKE_BATCH);
                            nr++;
                        }
                        io_uring_cq_advance(ring, nr);
                    }
                    else
                    {
                        // Timeout or error
                        if (has_timer)
                            timed_out = true;
                    }
                }
            }
            else
#endif // APTH_USE_IOURING
            {
                // ---- epoll path ----
                struct epoll_event ep_events[64];
                int nready = epoll_wait(sched->epoll_fd, ep_events, 64, timeout_ms);

                if (nready > 0)
                {
                    for (int i = 0; i < nready; i++)
                    {
                        if (ep_events[i].data.fd == sched->wake_eventfd)
                        {
                            uint64_t val;
                            ssize_t __ignored = apth_func_raw(read)(sched->wake_eventfd, &val, sizeof(val));
                            (void)__ignored;
                        }
                        else
                        {
                            epoll_map_wake_fd(sched, ep_events[i].data.fd, ep_events[i].events,
                                              fd_wake_batch, &fd_wake_count, MAX_WAKE_BATCH);
                        }
                    }
                }
                else if (nready == 0 && !dopoll && has_timer)
                {
                    timed_out = true;
                }
            }

            // --- Common post-processing for both backends ---

            // Transfer FD-waked threads directly to READY queue
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
                        fd_map_remove_waiter(sched, event->ev_args.FD.fd, th, event);
                }

                atomic_store_release(&th->state, APTH_STATE_READY);
                transfer_th(th, THQUEUE(sched, waiting), THQUEUE(sched, ready));
            }

            // Handle timer timeout
            if (timed_out && nexttimer_ev != NULL)
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
                            fd_map_remove_waiter(sched, event->ev_args.FD.fd, nexttimer_th, event);
                    }
                    atomic_store_release(&nexttimer_th->state, APTH_STATE_READY);
                    transfer_th(nexttimer_th, THQUEUE(sched, waiting), THQUEUE(sched, ready));
                }
            }
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
                fd_map_add_waiter(sched, pre_ev->ev_args.FD.fd, self, pre_ev);
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
        fd_map_add_waiter(SCHED_OF(self), ev->ev_args.FD.fd, self, ev);
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
