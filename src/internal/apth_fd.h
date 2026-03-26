#ifndef __LIBAPTH_INTERNAL_APTH_FD_H
#define __LIBAPTH_INTERNAL_APTH_FD_H

#include "utils/archplattoold.h"
#include <stdbool.h>
#include <sys/select.h>

// Filedescriptor blocking modes
enum
{
    APTH_FDMODE_ERROR = -1,
    APTH_FDMODE_POLL = 0,
    APTH_FDMODE_BLOCK,
    APTH_FDMODE_NONBLOCK
};

struct apth_fd_entry
{
    _Atomic(int) managed; // Whether this filedescriptor is managed by libapth
};

// Snapshot of the FD table for safe concurrent reads.
// Readers load this atomically; writers create a new snapshot and defer
// freeing the old one via apth_fd_table_reclaim().
struct apth_fd_table_snapshot
{
    struct apth_fd_entry *entries;
    int capacity;
};

// The current snapshot, accessed atomically by readers.
extern _Atomic(struct apth_fd_table_snapshot *) APTH_FD_SNAPSHOT;

// Convenience accessors (load snapshot once, then use it)
INLINE struct apth_fd_table_snapshot *apth_fd_snapshot_load(void)
{
    return __atomic_load_n(&APTH_FD_SNAPSHOT, __ATOMIC_ACQUIRE);
}

// Maximum number of old snapshots to defer-free
#define APTH_FD_TABLE_DEFER_MAX 8

// Initial capacity matches FD_SETSIZE for backwards compatibility
#define APTH_FD_TABLE_INIT_CAPACITY FD_SETSIZE

APTH_INTERNAL void apth_fd_table_init(void);
APTH_INTERNAL void apth_fd_table_destroy(void);
APTH_INTERNAL void apth_fd_ensure_capacity(int fd); // Grow table if needed
APTH_INTERNAL bool apth_fd_is_managed(int fd);      // Check if fd is in table and managed
APTH_INTERNAL void apth_fd_register(int fd);         // Register when socket/open
APTH_INTERNAL void apth_fd_unregister(int fd);       // Unregister when close
// TODO: we will remove this one day, when we fully hacked GLIBC
APTH_INTERNAL void apth_fd_register_optional(int fd);
APTH_INTERNAL void apth_notify_fd_closed(int fd); // Notify all schedulers about fd close

APTH_INTERNAL bool apth_util_fd_valid(int fd);
APTH_INTERNAL void apth_util_fds_merge(int nfd,
                                       fd_set *ifds1, fd_set *ofds1,
                                       fd_set *ifds2, fd_set *ofds2,
                                       fd_set *ifds3, fd_set *ofds3);
APTH_INTERNAL bool apth_util_fds_test(int nfd,
                                      fd_set *ifds1, fd_set *ofds1,
                                      fd_set *ifds2, fd_set *ofds2,
                                      fd_set *ifds3, fd_set *ofds3);
APTH_INTERNAL int apth_util_fds_select(int nfd,
                                       fd_set *ifds1, fd_set *ofds1,
                                       fd_set *ifds2, fd_set *ofds2,
                                       fd_set *ifds3, fd_set *ofds3);

#endif // __LIBAPTH_INTERNAL_APTH_FD_H
