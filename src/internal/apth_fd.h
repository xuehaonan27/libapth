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
    int orig_flags;       // Original fcntl flags
    _Atomic(int) managed; // Whether this filedescriptor is managed by libapth
};

#define APTH_FD_TABLE_SIZE FD_SETSIZE

extern struct apth_fd_entry APTH_FD_TABLE[APTH_FD_TABLE_SIZE];

APTH_INTERNAL void apth_fd_table_init(void);
APTH_INTERNAL void apth_fd_register(int fd);   // Register when socket/open
APTH_INTERNAL void apth_fd_unregister(int fd); // Unregister when close
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
