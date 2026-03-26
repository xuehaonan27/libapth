#ifndef __LIBAPTH_INTERNAL_APTH_IOURING_H
#define __LIBAPTH_INTERNAL_APTH_IOURING_H

#ifdef APTH_USE_IOURING

#include <liburing.h>
#include <stdbool.h>
#include "apth.h"
#include "utils/archplattoold.h"

// Per-scheduler io_uring instance
struct apth_iouring_ctx
{
    struct io_uring ring;
    bool initialized;
};

// Sentinel user_data values for CQE identification.
// Valid waiter pointers are always >= page size (4096+), so these are safe.
#define URING_UD_IGNORE  0ULL  // Cancel CQE or internal — ignore
#define URING_UD_WAKE    1ULL  // wake_eventfd POLL_ADD completion

// Probe whether io_uring is usable on this kernel/system.
// Performs a one-time probe (create + destroy a test ring).
// Returns true if io_uring can be used, false otherwise.
APTH_INTERNAL bool apth_iouring_available(void);

// Initialize a per-scheduler io_uring ring.
// queue_depth: number of SQ entries (rounded up to power of 2 by kernel).
// Returns 0 on success, negative errno on failure.
APTH_INTERNAL int apth_iouring_init(struct apth_iouring_ctx *ctx, int queue_depth);

// Destroy a per-scheduler io_uring ring.
APTH_INTERNAL void apth_iouring_destroy(struct apth_iouring_ctx *ctx);

// Returns true if the kernel supports FAST_POLL (5.7+).
// When FAST_POLL is available, IORING_OP_READ/WRITE on non-blocking FDs
// will internally poll and retry, allowing direct I/O submission.
APTH_INTERNAL bool apth_iouring_has_fast_poll(void);

// ==================== Direct I/O functions ====================
// These submit io_uring operations directly (IORING_OP_READ/WRITE/RECV/SEND/ACCEPT)
// instead of the poll-then-retry pattern.  Requires FAST_POLL support.
// Each function yields to the scheduler and returns the CQE result.
// Returns bytes transferred (>=0) or -1 with errno set.

APTH_INTERNAL ssize_t apth_uring_direct_read(int fd, void *buf, size_t count);
APTH_INTERNAL ssize_t apth_uring_direct_write(int fd, const void *buf, size_t count);
APTH_INTERNAL ssize_t apth_uring_direct_recv(int fd, void *buf, size_t len, int flags);
APTH_INTERNAL ssize_t apth_uring_direct_send(int fd, const void *buf, size_t len, int flags);
APTH_INTERNAL int     apth_uring_direct_accept(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags);

#endif // APTH_USE_IOURING

#endif // __LIBAPTH_INTERNAL_APTH_IOURING_H
