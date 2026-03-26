#ifndef __LIBAPTH_INTERNAL_APTH_EPOLL_WAITER_H
#define __LIBAPTH_INTERNAL_APTH_EPOLL_WAITER_H

#include "apth.h"
#include "internal/forward_declare.h"
#include "utils/list.h"

// Entry for an apth waiting for a certain fd.
// This structure should be linked into `epoll_fd_slot`'s waiting list
struct apth_epoll_waiter
{
    apth_t th;             // waiting apth
    apth_event_t ev;       // corresponding event, for marking the event as OCCURRED
    struct list_elem elem; // link into epoll_fd_slot.waiters or pending_cancels
#ifdef APTH_USE_IOURING
    int fd;                // FD this waiter is monitoring (needed for CQE processing)
    bool cancelled;        // true if cancel submitted; CQE will free
    bool direct_io;        // true for IORING_OP_READ/WRITE/etc. (result in ev->uring_io_result)
#endif
#define apth_epoll_waiter_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_epoll_waiter, elem)
};

#endif // __LIBAPTH_INTERNAL_APTH_EPOLL_WAITER_H
