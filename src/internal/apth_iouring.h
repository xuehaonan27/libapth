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

#endif // APTH_USE_IOURING

#endif // __LIBAPTH_INTERNAL_APTH_IOURING_H
