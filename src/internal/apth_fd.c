#include "apth_fd.h"
#include "internal/apth_global_sched_pool.h"
#include "internal/apth_sched.h"
#include "utils/atomic_wrapper.h"
#include "utils/debug.h"
#include "utils/lll.inline.h"
#include <fcntl.h>
#include <string.h>

struct apth_fd_entry APTH_FD_TABLE[APTH_FD_TABLE_SIZE];

APTH_INTERNAL void apth_fd_table_init(void)
{
    memset(APTH_FD_TABLE, 0, sizeof(APTH_FD_TABLE));
    apth_fd_register(0);
    apth_fd_register(1);
    apth_fd_register(2);
}

APTH_INTERNAL void apth_fd_register(int fd)
{
    if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
        return;

    // Get current flags
    int flags = apth_func_raw(fcntl)(fd, F_GETFL, 0);
    if (flags == -1)
        return;

    // Store original flags (for informational purposes only)
    APTH_FD_TABLE[fd].orig_flags = flags;

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
    apth_debug("Registered fd=%d (orig_flags=0x%x) now flags=0x%x", fd, flags, apth_func_raw(fcntl)(fd, F_GETFL, 0));
}

APTH_INTERNAL void apth_fd_unregister(int fd)
{
    if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
        return;

    // Just mark as unmanaged - don't restore flags
    // The FD is being closed anyway, so no point in restoring
    atomic_store_release(&APTH_FD_TABLE[fd].managed, 0);
    apth_debug("Unregistered fd=%d", fd);
}

APTH_INTERNAL void apth_fd_register_optional(int fd)
{
    if (atomic_load_acquire(&APTH_FD_TABLE[fd].managed) == 0)
    {
        // Not managed but we are going to use it
        apth_fd_register(fd);
    }
}

// // Before performing I/O operation. Set to NONBLOCK if the fd is used first time
// APTH_INTERNAL int apth_fd_acquire(int fd)
// {
//     if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
//         return -1;

//     struct apth_fd_entry *e = &APTH_FD_TABLE[fd];

//     // Check if this is the first time we're seeing this fd (refcount == 0)
//     int old_ref = atomic_fetch_add_acq_rel(&e->refcount, 1);

//     if (old_ref == 0)
//     {
//         // First user of this fd - need to set it to NONBLOCK
//         // This happens either on first use, or after fd was closed and reopened
//         e->orig_flags = fcntl(fd, F_GETFL, 0);
//         if (e->orig_flags == -1)
//         {
//             // Invalid fd - decrement refcount and return error
//             atomic_fetch_sub_acq_rel(&e->refcount, 1);
//             return -1;
//         }

//         // Set to NONBLOCK if not already
//         if (!(e->orig_flags & O_NONBLOCK))
//         {
//             if (fcntl(fd, F_SETFL, e->orig_flags | O_NONBLOCK) == -1)
//             {
//                 // fcntl failed - decrement refcount and return error
//                 atomic_fetch_sub_acq_rel(&e->refcount, 1);
//                 return -1;
//             }
//         }

//         atomic_store_release(&e->managed, 1);
//     }

//     return (e->orig_flags & O_NONBLOCK) ? APTH_FDMODE_NONBLOCK : APTH_FDMODE_BLOCK;
// }

// // Perform after I/O operation: just decrement refcount
// // No need to restore flags here - fd stays NONBLOCK while managed
// // Flags are only restored in apth_fd_unregister() when fd is closed
// APTH_INTERNAL void apth_fd_release(int fd)
// {
//     if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
//         return;

//     struct apth_fd_entry *e = &APTH_FD_TABLE[fd];

//     // Just decrement refcount, no fcntl() needed
//     // This eliminates syscalls in the hot path
//     atomic_fetch_sub_acq_rel(&e->refcount, 1);
// }

// Notify ALL schedulers (including the caller's own) that `fd` has been closed.
// Each scheduler will process the notification at the start of its next event
// manager iteration, failing all local waiters for this fd.
APTH_INTERNAL void apth_notify_fd_closed(int fd)
{
    if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
        return;

    apth_debug("notifying all schedulers: fd=%d closed", fd);

    lll_internal_lock(&GLOBAL_POOL.pool_lock);
    FOR_ELEMENT_IN_LIST(GLOBAL_POOL.wrkpthrs_list, e)
    {
        apth_worker_t worker = apth_worker_t_list_entry(e);
        apth_sched_t sched = worker->sched;
        if (sched == NULL)
            continue;

        lll_internal_lock(&sched->pending_fd_close_lock);
        int idx = atomic_load_acquire(&sched->pending_fd_close_count);
        if (idx < APTH_PENDING_FD_CLOSE_MAX)
        {
            sched->pending_fd_close_fds[idx] = fd;
            atomic_store_release(&sched->pending_fd_close_count, idx + 1);
        }
        else
        {
            apth_debug("WARNING: pending_fd_close overflow for sched %d, fd=%d dropped",
                       sched->id, fd);
        }
        lll_internal_unlock(&sched->pending_fd_close_lock);
    }
    lll_internal_unlock(&GLOBAL_POOL.pool_lock);
}

APTH_INTERNAL bool apth_util_fd_valid(int fd)
{
    if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
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
