#ifndef __LIBAPTH_INTERNAL_APTH_FD_SLOT_H
#define __LIBAPTH_INTERNAL_APTH_FD_SLOT_H

#include "utils/list.h"
#include <stdint.h>

// A fd slot monitored by epoll
struct apth_epoll_fd_slot
{
    int fd;                    // monitored fd
    uint32_t aggregate_events; // aggregation of all waiter's event mask（EPOLLIN|EPOLLOUT|...）
    struct list waiters;       // apth list waiting this fd [elem: struct apth_epoll_waiter]
    int waiter_count;          // number of waiters
    bool registered;           // already registered to epoll?
    struct list_elem elem;     // link into scheduler's active_fd_slots list
#define apth_epoll_fd_slot_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_epoll_fd_slot, elem)

    // Reference counts for each event type to avoid recalculation
    int readable_count;  // Number of waiters waiting for EPOLLIN
    int writeable_count; // Number of waiters waiting for EPOLLOUT
    int exception_count; // Number of waiters waiting for EPOLLPRI

    bool epoll_dirty;  // True if aggregate_events changed but epoll not yet updated
};

#define APTH_EPOLL_FD_SLOT_TABLE_SIZE FD_SETSIZE

#endif // __LIBAPTH_INTERNAL_APTH_FD_SLOT_H
