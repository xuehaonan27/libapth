#ifndef __LIBAPTH_INTERNAL_APTH_FD_SLOT_H
#define __LIBAPTH_INTERNAL_APTH_FD_SLOT_H

#include "utils/list.h"
#include <stdint.h>

// A fd slot monitored by epoll
struct apth_epoll_fd_slot
{
    int fd;                    // monitored fd
    struct list waiters;       // apth list waiting this fd [elem: struct apth_epoll_waiter]
    int waiter_count;          // number of waiters
    bool registered;           // already registered to epoll?
    struct list_elem elem;     // link into scheduler's active_fd_slots list
#define apth_epoll_fd_slot_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_epoll_fd_slot, elem)
};

#define APTH_EPOLL_FD_SLOT_TABLE_SIZE FD_SETSIZE

#endif // __LIBAPTH_INTERNAL_APTH_FD_SLOT_H
