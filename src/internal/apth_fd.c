#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/atomic_wrapper.h"
#include "utils/debug.h"
#include <fcntl.h>
#include <string.h>

struct apth_fd_entry APTH_FD_TABLE[APTH_FD_TABLE_SIZE];

APTH_INTERNAL void apth_fd_table_init(void)
{
    memset(APTH_FD_TABLE, 0, sizeof(APTH_FD_TABLE));
}

APTH_INTERNAL void apth_fd_register(int fd)
{
    if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
        return;
    int flags = fcntl(fd, F_GETFL, 0);
    APTH_FD_TABLE[fd].orig_flags = flags;
    atomic_store_release(&APTH_FD_TABLE[fd].managed, 1);
    atomic_store_release(&APTH_FD_TABLE[fd].refcount, 0);
}

APTH_INTERNAL void apth_fd_unregister(int fd)
{
    if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
        return;
    // Restore original flags
    // (if there's still someone using this fd, meaning rc > 0 but
    // theoratically this shouldn't happen)
    if (atomic_load_acquire(&APTH_FD_TABLE[fd].managed))
    {
        fcntl(fd, F_SETFL, APTH_FD_TABLE[fd].orig_flags);
        atomic_store_release(&APTH_FD_TABLE[fd].managed, 0);
        atomic_store_release(&APTH_FD_TABLE[fd].refcount, 0);
    }
}

// Before performing I/O operation. Set to NONBLOCK if the fd is used first time
APTH_INTERNAL int apth_fd_acquire(int fd)
{
    if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
        return -1;

    struct apth_fd_entry *e = &APTH_FD_TABLE[fd];

    // Check if this is the first time we're seeing this fd (refcount == 0)
    int old_ref = atomic_fetch_add_acq_rel(&e->refcount, 1);

    if (old_ref == 0)
    {
        // First user of this fd - need to set it to NONBLOCK
        // This happens either on first use, or after fd was closed and reopened
        e->orig_flags = fcntl(fd, F_GETFL, 0);
        if (e->orig_flags == -1)
        {
            // Invalid fd - decrement refcount and return error
            atomic_fetch_sub_acq_rel(&e->refcount, 1);
            return -1;
        }

        // Set to NONBLOCK if not already
        if (!(e->orig_flags & O_NONBLOCK))
        {
            if (fcntl(fd, F_SETFL, e->orig_flags | O_NONBLOCK) == -1)
            {
                // fcntl failed - decrement refcount and return error
                atomic_fetch_sub_acq_rel(&e->refcount, 1);
                return -1;
            }
        }

        atomic_store_release(&e->managed, 1);
    }

    return (e->orig_flags & O_NONBLOCK) ? APTH_FDMODE_NONBLOCK : APTH_FDMODE_BLOCK;
}

// Perform after I/O operation: just decrement refcount
// No need to restore flags here - fd stays NONBLOCK while managed
// Flags are only restored in apth_fd_unregister() when fd is closed
APTH_INTERNAL void apth_fd_release(int fd)
{
    if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
        return;

    struct apth_fd_entry *e = &APTH_FD_TABLE[fd];

    // Just decrement refcount, no fcntl() needed
    // This eliminates syscalls in the hot path
    atomic_fetch_sub_acq_rel(&e->refcount, 1);
}

// Notify ALL schedulers (including the caller's own) that `fd` has been closed.
// Each scheduler will process the notification at the start of its next event
// manager iteration, failing all local waiters for this fd.
APTH_INTERNAL void apth_notify_fd_closed(int fd)
{
    if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
        return;

    apth_debug("notifying all schedulers: fd=%d closed", fd);

    lll_lock(&GLOBAL_POOL.pool_lock, "apth_notify_fd_closed");
    FOR_ELEMENT_IN_LIST(GLOBAL_POOL.wrkpthrs_list, e)
    {
        apth_worker_t worker = apth_worker_t_list_entry(e);
        apth_sched_t sched = worker->sched;
        if (sched == NULL)
            continue;

        lll_lock(&sched->pending_fd_close_lock, "notify_fd_closed_per_sched");
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
        lll_unlock(&sched->pending_fd_close_lock, "notify_fd_closed_per_sched");
    }
    lll_unlock(&GLOBAL_POOL.pool_lock, "apth_notify_fd_closed");
}
