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
    // if (!atomic_load_acquire(&e->managed))
    if (atomic_compare_exchange_strong(&e->managed, &(int){0}, 1))
    {
        // fd meet first time, maybe a fd opened by user outside libapth
        e->orig_flags = fcntl(fd, F_GETFL, 0);
        if (e->orig_flags == -1)
            return -1;
        atomic_store_release(&e->managed, 1);
    }

    // TODO: should this use `atomic_fetch_add_acq_rel`, or `atomic_fetch_add` which
    // has stronger SEQCST semantics ?
    int old_ref = atomic_fetch_add_acq_rel(&e->refcount, 1);
    if (old_ref == 0)
    {
        // First user, set to NONBLOCK
        if (!(e->orig_flags & O_NONBLOCK))
            fcntl(fd, F_SETFL, e->orig_flags | O_NONBLOCK);
    }
    // If refcount > 0, then fd should already be NONBLOCK, no fcntl needed
    return (e->orig_flags & O_NONBLOCK) ? APTH_FDMODE_NONBLOCK : APTH_FDMODE_BLOCK;
}

// Perform after I/O operation: restore original flags when reference count is 0
APTH_INTERNAL void apth_fd_release(int fd)
{
    if (fd < 0 || fd >= APTH_FD_TABLE_SIZE)
        return;

    struct apth_fd_entry *e = &APTH_FD_TABLE[fd];

    // TODO: should this use `atomic_fetch_sub_acq_rel`, or `atomic_fetch_sub` which
    // has stronger SEQCST semantics ?
    int new_ref = atomic_fetch_sub_acq_rel(&e->refcount, 1) - 1;
    if (new_ref == 0)
    {
        // Last user, restore original flags
        if (!(e->orig_flags & O_NONBLOCK))
            fcntl(fd, F_SETFL, e->orig_flags);
    }
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
