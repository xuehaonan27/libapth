#ifndef __LIBAPTH_INTERNAL_APTH_DEDICATED_H
#define __LIBAPTH_INTERNAL_APTH_DEDICATED_H

#include "internal/forward_declare.h"
#include "utils/lll.h"
#include "utils/list.h"

// Wrapper function for dedicated thread pthreads.
// This is the start_routine passed to pthread_create for dedicated threads.
APTH_INTERNAL void *apth_dedicated_thread_wrapper(void *arg);

// Exit path for dedicated threads (no scheduler involvement).
// Runs cleanup handlers, TLS destructors, signals joiner, sets TERMINATED.
APTH_INTERNAL void apth_dedicated_do_exit(void *result);

// Block a dedicated thread. Blocks the calling pthread on the thread's
// dedicated futex. Used by sync primitives when a dedicated thread must wait.
APTH_INTERNAL void apth_dedicated_block(apth_t t);

// Block a dedicated thread until an absolute CLOCK_REALTIME deadline. Returns
// 0 for a wake/spurious wake and ETIMEDOUT if the deadline has passed.
APTH_INTERNAL int apth_dedicated_block_until(apth_t t, const struct timespec *abstime);

// Unblock a dedicated thread. Wakes the thread's dedicated futex.
// Safe to call from any context
// (regular apth, scheduler, or another dedicated thread).
APTH_INTERNAL void apth_dedicated_unblock(apth_t t);

// Dedicated thread registry: tracks all live dedicated threads for
// enumeration and graceful shutdown.
extern lll_internal_t __dedicated_registry_lock;
extern struct list __dedicated_registry;

// Initialize the dedicated thread registry (called from apth_init_common).
APTH_INTERNAL void apth_dedicated_registry_init(void);

#endif // __LIBAPTH_INTERNAL_APTH_DEDICATED_H
