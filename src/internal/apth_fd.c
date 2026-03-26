#include "apth_fd.h"
#include "internal/apth_global_sched_pool.h"
#include "internal/apth_sched.h"
#include "internal/types.h"
#include "utils/atomic_wrapper.h"
#include "utils/debug.h"
#include "utils/lll.inline.h"
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

/* ====================================================================
 * Deferred-free FD table
 *
 * The FD table must grow dynamically when FDs exceed capacity.  Readers
 * (I/O hooks on every scheduler) access the table without locking for
 * performance.  To avoid use-after-free when the table is replaced:
 *
 *   1. The current table is wrapped in a snapshot struct (entries + capacity).
 *   2. Readers load the snapshot pointer atomically, then use it.
 *   3. Writers create a new snapshot (copy + extend), atomically publish it,
 *      and defer-free the old snapshot.  The old snapshot is freed on the
 *      NEXT grow operation (by which time all readers have moved on, since
 *      each reader loads the snapshot pointer fresh on every access).
 *
 * This is a simplified RCU: one deferred generation is sufficient because
 * grow operations are rare and readers are non-reentrant short-lived
 * fast-path checks.
 * ==================================================================== */

_Atomic(struct apth_fd_table_snapshot *) APTH_FD_SNAPSHOT = NULL;

/* Deferred-free ring: old snapshots waiting to be freed. */
static struct apth_fd_table_snapshot *deferred_free[APTH_FD_TABLE_DEFER_MAX];
static int deferred_free_count = 0;

/* Lock protecting table growth and deferred-free (rare operation) */
static lll_internal_t fd_table_grow_lock;

/* Free all deferred snapshots.  Called under grow_lock. */
static void flush_deferred(void)
{
    for (int i = 0; i < deferred_free_count; i++)
    {
        free(deferred_free[i]->entries);
        free(deferred_free[i]);
        deferred_free[i] = NULL;
    }
    deferred_free_count = 0;
}

APTH_INTERNAL void apth_fd_table_init(void)
{
    struct apth_fd_table_snapshot *snap = malloc(sizeof(*snap));
    if (snap == NULL)
        PANIC("Failed to allocate FD table snapshot");

    snap->capacity = APTH_FD_TABLE_INIT_CAPACITY;
    snap->entries = (struct apth_fd_entry *)calloc(snap->capacity, sizeof(struct apth_fd_entry));
    if (snap->entries == NULL)
        PANIC("Failed to allocate FD table");

    __atomic_store_n(&APTH_FD_SNAPSHOT, snap, __ATOMIC_RELEASE);
    lll_internal_init(&fd_table_grow_lock);

    apth_fd_register(0);
    apth_fd_register(1);
    apth_fd_register(2);
}

APTH_INTERNAL void apth_fd_table_destroy(void)
{
    struct apth_fd_table_snapshot *snap = __atomic_load_n(&APTH_FD_SNAPSHOT, __ATOMIC_ACQUIRE);
    if (snap != NULL)
    {
        free(snap->entries);
        free(snap);
        __atomic_store_n(&APTH_FD_SNAPSHOT, NULL, __ATOMIC_RELEASE);
    }
    flush_deferred();
}

/* Grow the table to accommodate fd.  Must be called under grow_lock. */
static void fd_table_grow_to(int min_capacity)
{
    struct apth_fd_table_snapshot *old_snap = __atomic_load_n(&APTH_FD_SNAPSHOT, __ATOMIC_ACQUIRE);
    int new_cap = old_snap->capacity;
    while (new_cap <= min_capacity)
        new_cap *= 2;

    /* Allocate new snapshot */
    struct apth_fd_table_snapshot *new_snap = malloc(sizeof(*new_snap));
    if (new_snap == NULL)
        PANIC("Failed to allocate FD table snapshot");

    new_snap->capacity = new_cap;
    new_snap->entries = (struct apth_fd_entry *)calloc(new_cap, sizeof(struct apth_fd_entry));
    if (new_snap->entries == NULL)
        PANIC("Failed to grow FD table to %d", new_cap);

    /* Copy existing entries */
    memcpy(new_snap->entries, old_snap->entries,
           old_snap->capacity * sizeof(struct apth_fd_entry));

    /* Publish new snapshot atomically */
    __atomic_store_n(&APTH_FD_SNAPSHOT, new_snap, __ATOMIC_RELEASE);

    /* Defer-free the old snapshot.  Flush oldest entries if ring is full. */
    if (deferred_free_count >= APTH_FD_TABLE_DEFER_MAX)
        flush_deferred();
    deferred_free[deferred_free_count++] = old_snap;

    apth_debug("FD table grown to capacity %d", new_cap);
}

APTH_INTERNAL void apth_fd_ensure_capacity(int fd)
{
    struct apth_fd_table_snapshot *snap = apth_fd_snapshot_load();
    if (fd < snap->capacity)
        return;

    lll_internal_lock(&fd_table_grow_lock);
    /* Re-check under lock (another thread may have grown it) */
    snap = apth_fd_snapshot_load();
    if (fd >= snap->capacity)
        fd_table_grow_to(fd);
    lll_internal_unlock(&fd_table_grow_lock);
}

APTH_INTERNAL bool apth_fd_is_managed(int fd)
{
    if (fd < 0)
        return false;
    struct apth_fd_table_snapshot *snap = apth_fd_snapshot_load();
    if (fd >= snap->capacity)
        return false;
    return atomic_load_acquire(&snap->entries[fd].managed) != 0;
}

APTH_INTERNAL void apth_fd_register(int fd)
{
    if (fd < 0)
        return;

    /* Grow table if needed */
    apth_fd_ensure_capacity(fd);

    struct apth_fd_table_snapshot *snap = apth_fd_snapshot_load();

    /* Get current flags */
    int flags = apth_func_raw(fcntl)(fd, F_GETFL, 0);
    if (flags == -1)
        return;

    /* Set to non-blocking if not already */
    if (!(flags & O_NONBLOCK))
    {
        if (apth_func_raw(fcntl)(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            apth_debug("Failed to set O_NONBLOCK on fd=%d", fd);
            PANIC("Failed to set O_NONBLOCK on fd=%d", fd);
            return;
        }
    }

    /* Mark as managed */
    atomic_store_release(&snap->entries[fd].managed, 1);
#ifdef APTH_DEBUG
    apth_debug("Registered fd=%d (orig_flags=0x%x) now flags=0x%x",
               fd, flags, apth_func_raw(fcntl)(fd, F_GETFL, 0));
#endif
}

/* Fast-path registration for FDs already created with O_NONBLOCK
 * (via SOCK_NONBLOCK, accept4, pipe2, O_NONBLOCK in open flags).
 * Skips the two fcntl syscalls that apth_fd_register does. */
APTH_INTERNAL void apth_fd_register_nonblock(int fd)
{
    if (fd < 0)
        return;
    apth_fd_ensure_capacity(fd);
    struct apth_fd_table_snapshot *snap = apth_fd_snapshot_load();
    atomic_store_release(&snap->entries[fd].managed, 1);
    apth_debug("Registered fd=%d (already nonblock)", fd);
}

APTH_INTERNAL void apth_fd_unregister(int fd)
{
    if (fd < 0)
        return;
    struct apth_fd_table_snapshot *snap = apth_fd_snapshot_load();
    if (fd >= snap->capacity)
        return;

    atomic_store_release(&snap->entries[fd].managed, 0);
    apth_debug("Unregistered fd=%d", fd);
}

APTH_INTERNAL void apth_fd_register_optional(int fd)
{
    if (fd < 0)
        return;
    if (!apth_fd_is_managed(fd))
    {
        apth_fd_register(fd);
    }
}

/* Notify that fd has been closed. */
APTH_INTERNAL void apth_notify_fd_closed(int fd)
{
    if (fd < 0)
        return;

    /* Push fd to each scheduler's pending close list.
     * We do NOT call apth_sched_wake() here — each scheduler processes
     * pending_fd_closes at the top of its event manager loop anyway.
     * Skipping the wake avoids N write(eventfd) syscalls per close(),
     * which was the single largest source of syscall overhead in the
     * HTTP server benchmark (8 syscalls per request for 2 closes × 4
     * schedulers). The trade-off is that cross-scheduler cleanup may
     * be delayed by up to one event loop iteration (~10ms worst case),
     * which is acceptable for a close notification. */
    apth_debug("notifying schedulers: fd=%d closed", fd);
    int n_workers = GLOBAL_POOL.init_worker_count;
    for (int i = 0; i < n_workers; i++)
    {
        apth_worker_t w = GLOBAL_POOL.worker_ptr_mem_start[i];
        if (w == NULL)
            continue;
        apth_sched_t sched = w->sched;
        if (sched == NULL)
            continue;

        lll_internal_lock(&sched->pending_fd_close_lock);
        int idx = atomic_load_acquire(&sched->pending_fd_close_count);
        if (idx < APTH_PENDING_FD_CLOSE_MAX)
        {
            sched->pending_fd_close_fds[idx] = fd;
            atomic_store_release(&sched->pending_fd_close_count, idx + 1);
        }
        lll_internal_unlock(&sched->pending_fd_close_lock);
    }
}

APTH_INTERNAL bool apth_util_fd_valid(int fd)
{
    if (fd < 0)
        return false;
    int flags = apth_func_raw(fcntl)(fd, F_GETFL);
    if (flags == -1 && errno == EBADF)
        return false;
    if ((flags & O_NONBLOCK) == 0)
    {
        PANIC("fd=%d not in NONBLOCK mode!", fd);
    }
    return true;
}

/* Number of words needed to cover nfd file descriptors */
#if defined(__linux__) && defined(__NFDBITS)
#define FDS_NWORDS(nfd) (((nfd) + __NFDBITS - 1) / __NFDBITS)
#endif

APTH_INTERNAL void apth_util_fds_merge(int nfd,
                                       fd_set *ifds1, fd_set *ofds1,
                                       fd_set *ifds2, fd_set *ofds2,
                                       fd_set *ifds3, fd_set *ofds3)
{
#if defined(__linux__) && defined(__NFDBITS)
    int nwords = FDS_NWORDS(nfd);
    for (int i = 0; i < nwords; i++)
    {
        if (ifds1 != NULL)
            ofds1->fds_bits[i] |= ifds1->fds_bits[i];
        if (ifds2 != NULL)
            ofds2->fds_bits[i] |= ifds2->fds_bits[i];
        if (ifds3 != NULL)
            ofds3->fds_bits[i] |= ifds3->fds_bits[i];
    }
#else
    for (int s = 0; s < nfd; s++)
    {
        if (ifds1 != NULL && FD_ISSET(s, ifds1))
            FD_SET(s, ofds1);
        if (ifds2 != NULL && FD_ISSET(s, ifds2))
            FD_SET(s, ofds2);
        if (ifds3 != NULL && FD_ISSET(s, ifds3))
            FD_SET(s, ofds3);
    }
#endif
}

APTH_INTERNAL bool apth_util_fds_test(int nfd,
                                      fd_set *ifds1, fd_set *ofds1,
                                      fd_set *ifds2, fd_set *ofds2,
                                      fd_set *ifds3, fd_set *ofds3)
{
#if defined(__linux__) && defined(__NFDBITS)
    int nwords = FDS_NWORDS(nfd);
    for (int i = 0; i < nwords; i++)
    {
        if (ifds1 != NULL && (ifds1->fds_bits[i] & ofds1->fds_bits[i]))
            return true;
        if (ifds2 != NULL && (ifds2->fds_bits[i] & ofds2->fds_bits[i]))
            return true;
        if (ifds3 != NULL && (ifds3->fds_bits[i] & ofds3->fds_bits[i]))
            return true;
    }
    return false;
#else
    for (int s = 0; s < nfd; s++)
    {
        if (ifds1 != NULL && FD_ISSET(s, ifds1) && FD_ISSET(s, ofds1))
            return true;
        if (ifds2 != NULL && FD_ISSET(s, ifds2) && FD_ISSET(s, ofds2))
            return true;
        if (ifds3 != NULL && FD_ISSET(s, ifds3) && FD_ISSET(s, ofds3))
            return true;
    }
    return false;
#endif
}

APTH_INTERNAL int apth_util_fds_select(int nfd,
                                       fd_set *ifds1, fd_set *ofds1,
                                       fd_set *ifds2, fd_set *ofds2,
                                       fd_set *ifds3, fd_set *ofds3)
{
    int n = 0;

#if defined(__linux__) && defined(__NFDBITS)
    int nwords = FDS_NWORDS(nfd);
    for (int i = 0; i < nwords; i++)
    {
        if (ifds1 != NULL)
        {
            __fd_mask kept = ifds1->fds_bits[i] & ofds1->fds_bits[i];
            __fd_mask cleared = ifds1->fds_bits[i] & ~kept;
            ifds1->fds_bits[i] &= ~cleared;
            n += __builtin_popcountl(kept);
        }
        if (ifds2 != NULL)
        {
            __fd_mask kept = ifds2->fds_bits[i] & ofds2->fds_bits[i];
            __fd_mask cleared = ifds2->fds_bits[i] & ~kept;
            ifds2->fds_bits[i] &= ~cleared;
            n += __builtin_popcountl(kept);
        }
        if (ifds3 != NULL)
        {
            __fd_mask kept = ifds3->fds_bits[i] & ofds3->fds_bits[i];
            __fd_mask cleared = ifds3->fds_bits[i] & ~kept;
            ifds3->fds_bits[i] &= ~cleared;
            n += __builtin_popcountl(kept);
        }
    }
#else
    for (int s = 0; s < nfd; s++)
    {
        if (ifds1 != NULL && FD_ISSET(s, ifds1))
        {
            if (!FD_ISSET(s, ofds1))
                FD_CLR(s, ifds1);
            else
                n++;
        }
        if (ifds2 != NULL && FD_ISSET(s, ifds2))
        {
            if (!FD_ISSET(s, ofds2))
                FD_CLR(s, ifds2);
            else
                n++;
        }
        if (ifds3 != NULL && FD_ISSET(s, ifds3))
        {
            if (!FD_ISSET(s, ofds3))
                FD_CLR(s, ifds3);
            else
                n++;
        }
    }
#endif

    return n;
}
