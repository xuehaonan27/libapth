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

// Dynamic FD table
struct apth_fd_entry *APTH_FD_TABLE = NULL;
int APTH_FD_TABLE_CAPACITY = 0;

// Lock protecting table growth (rare operation)
static lll_internal_t fd_table_grow_lock;

APTH_INTERNAL void apth_fd_table_init(void)
{
    APTH_FD_TABLE_CAPACITY = APTH_FD_TABLE_INIT_CAPACITY;
    APTH_FD_TABLE = (struct apth_fd_entry *)calloc(APTH_FD_TABLE_CAPACITY, sizeof(struct apth_fd_entry));
    if (APTH_FD_TABLE == NULL)
        PANIC("Failed to allocate FD table");
    lll_internal_init(&fd_table_grow_lock);
    apth_fd_register(0);
    apth_fd_register(1);
    apth_fd_register(2);
}

APTH_INTERNAL void apth_fd_table_destroy(void)
{
    free(APTH_FD_TABLE);
    APTH_FD_TABLE = NULL;
    APTH_FD_TABLE_CAPACITY = 0;
}

// Grow the table to accommodate fd. Must be called under grow lock.
static void fd_table_grow_to(int min_capacity)
{
    int new_cap = APTH_FD_TABLE_CAPACITY;
    while (new_cap <= min_capacity)
        new_cap *= 2;

    struct apth_fd_entry *new_table = (struct apth_fd_entry *)calloc(new_cap, sizeof(struct apth_fd_entry));
    if (new_table == NULL)
        PANIC("Failed to grow FD table to %d", new_cap);

    // Copy existing entries
    memcpy(new_table, APTH_FD_TABLE, APTH_FD_TABLE_CAPACITY * sizeof(struct apth_fd_entry));

    // Swap: old readers see stale pointer briefly but won't access beyond old capacity
    // because all access paths check capacity first. The lock serializes writers.
    struct apth_fd_entry *old_table = APTH_FD_TABLE;
    APTH_FD_TABLE = new_table;
    // Memory fence to ensure new pointer is visible before new capacity
    __atomic_thread_fence(__ATOMIC_RELEASE);
    APTH_FD_TABLE_CAPACITY = new_cap;

    free(old_table);
    apth_debug("FD table grown to capacity %d", new_cap);
}

APTH_INTERNAL void apth_fd_ensure_capacity(int fd)
{
    if (fd < APTH_FD_TABLE_CAPACITY)
        return;

    lll_internal_lock(&fd_table_grow_lock);
    // Re-check under lock (another thread may have grown it)
    if (fd >= APTH_FD_TABLE_CAPACITY)
        fd_table_grow_to(fd);
    lll_internal_unlock(&fd_table_grow_lock);
}

APTH_INTERNAL bool apth_fd_is_managed(int fd)
{
    if (fd < 0 || fd >= APTH_FD_TABLE_CAPACITY)
        return false;
    return atomic_load_acquire(&APTH_FD_TABLE[fd].managed) != 0;
}

APTH_INTERNAL void apth_fd_register(int fd)
{
    if (fd < 0)
        return;

    // Grow table if needed
    apth_fd_ensure_capacity(fd);

    // Get current flags
    int flags = apth_func_raw(fcntl)(fd, F_GETFL, 0);
    if (flags == -1)
        return;

    // Set to non-blocking if not already
    if (!(flags & O_NONBLOCK))
    {
        if (apth_func_raw(fcntl)(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            apth_debug("Failed to set O_NONBLOCK on fd=%d", fd);
            PANIC("Failed to set O_NONBLOCK on fd=%d", fd);
            return;
        }
    }

    // Mark as managed (no refcount needed)
    atomic_store_release(&APTH_FD_TABLE[fd].managed, 1);
#ifdef APTH_DEBUG
    apth_debug("Registered fd=%d (orig_flags=0x%x) now flags=0x%x", fd, flags, apth_func_raw(fcntl)(fd, F_GETFL, 0));
#endif
}

APTH_INTERNAL void apth_fd_unregister(int fd)
{
    if (fd < 0 || fd >= APTH_FD_TABLE_CAPACITY)
        return;

    // Just mark as unmanaged - don't restore flags
    // The FD is being closed anyway, so no point in restoring
    atomic_store_release(&APTH_FD_TABLE[fd].managed, 0);
    apth_debug("Unregistered fd=%d", fd);
}

APTH_INTERNAL void apth_fd_register_optional(int fd)
{
    if (fd < 0)
        return;
    if (!apth_fd_is_managed(fd))
    {
        // Not managed but we are going to use it
        apth_fd_register(fd);
    }
}

// Notify that `fd` has been closed.
APTH_INTERNAL void apth_notify_fd_closed(int fd)
{
    if (fd < 0)
        return;

    // Notify all schedulers by pushing fd to each scheduler's pending close list
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

        apth_sched_wake(sched);
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

// Number of words needed to cover nfd file descriptors
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

// test whether fds in the input fd sets occurred in the output fds
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

// Clear fds in input fd sets if not occurred in output fd sets and return
// number of remaining input fds. This number uses BSD select(2) semantics: a
// fd in two set counts twice!
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
            // Keep only bits that are in both input and output
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
