#ifndef __LIBAPTH_INTERNAL_STATS_H
#define __LIBAPTH_INTERNAL_STATS_H

#include <stdint.h>
#include <stdio.h>

// Lightweight per-hook counters for profiling LIBAPTH overhead.
// Enabled by defining APTH_PROFILE_HOOKS at compile time.
// Reported on apth_drop() or via apth_stats_dump().

#ifdef APTH_PROFILE_HOOKS

#include "utils/atomic_wrapper.h"

struct apth_hook_stats {
    _Atomic(uint64_t) calls_total;      // Total hook invocations
    _Atomic(uint64_t) calls_dedicated;  // Bypassed (DEDICATED or NULL thread)
    _Atomic(uint64_t) calls_mn;         // Full M:N hook path
    _Atomic(uint64_t) calls_yield;      // Yielded to scheduler (EAGAIN)
    _Atomic(uint64_t) eventfd_reads;    // DEDICATED thread blocking reads
    _Atomic(uint64_t) eventfd_writes;   // DEDICATED thread wakeup writes
};

// Per-hook-category counters
extern struct apth_hook_stats __apth_stats_read;
extern struct apth_hook_stats __apth_stats_write;
extern struct apth_hook_stats __apth_stats_open;
extern struct apth_hook_stats __apth_stats_close;
extern struct apth_hook_stats __apth_stats_socket;
extern struct apth_hook_stats __apth_stats_other;
extern _Atomic(uint64_t) __apth_stats_dedicated_creates;
extern _Atomic(uint64_t) __apth_stats_mn_creates;

#define APTH_STAT_INC(counter) atomic_fetch_add_relaxed(&(counter), 1)

static inline void apth_stats_dump(void) {
    fprintf(stderr, "\n=== LIBAPTH Hook Statistics ===\n");
    fprintf(stderr, "read:   total=%lu dedicated=%lu mn=%lu yield=%lu\n",
            (unsigned long)atomic_load_relaxed(&__apth_stats_read.calls_total),
            (unsigned long)atomic_load_relaxed(&__apth_stats_read.calls_dedicated),
            (unsigned long)atomic_load_relaxed(&__apth_stats_read.calls_mn),
            (unsigned long)atomic_load_relaxed(&__apth_stats_read.calls_yield));
    fprintf(stderr, "write:  total=%lu dedicated=%lu mn=%lu yield=%lu\n",
            (unsigned long)atomic_load_relaxed(&__apth_stats_write.calls_total),
            (unsigned long)atomic_load_relaxed(&__apth_stats_write.calls_dedicated),
            (unsigned long)atomic_load_relaxed(&__apth_stats_write.calls_mn),
            (unsigned long)atomic_load_relaxed(&__apth_stats_write.calls_yield));
    fprintf(stderr, "open:   total=%lu dedicated=%lu\n",
            (unsigned long)atomic_load_relaxed(&__apth_stats_open.calls_total),
            (unsigned long)atomic_load_relaxed(&__apth_stats_open.calls_dedicated));
    fprintf(stderr, "close:  total=%lu dedicated=%lu\n",
            (unsigned long)atomic_load_relaxed(&__apth_stats_close.calls_total),
            (unsigned long)atomic_load_relaxed(&__apth_stats_close.calls_dedicated));
    fprintf(stderr, "socket: total=%lu dedicated=%lu mn=%lu yield=%lu\n",
            (unsigned long)atomic_load_relaxed(&__apth_stats_socket.calls_total),
            (unsigned long)atomic_load_relaxed(&__apth_stats_socket.calls_dedicated),
            (unsigned long)atomic_load_relaxed(&__apth_stats_socket.calls_mn),
            (unsigned long)atomic_load_relaxed(&__apth_stats_socket.calls_yield));
    fprintf(stderr, "other:  total=%lu dedicated=%lu\n",
            (unsigned long)atomic_load_relaxed(&__apth_stats_other.calls_total),
            (unsigned long)atomic_load_relaxed(&__apth_stats_other.calls_dedicated));
    fprintf(stderr, "eventfd: reads=%lu writes=%lu\n",
            (unsigned long)atomic_load_relaxed(&__apth_stats_read.eventfd_reads),
            (unsigned long)atomic_load_relaxed(&__apth_stats_write.eventfd_writes));
    fprintf(stderr, "creates: dedicated=%lu mn=%lu\n",
            (unsigned long)atomic_load_relaxed(&__apth_stats_dedicated_creates),
            (unsigned long)atomic_load_relaxed(&__apth_stats_mn_creates));
    fprintf(stderr, "=== End LIBAPTH Stats ===\n\n");
}

#else

#define APTH_STAT_INC(counter) ((void)0)
static inline void apth_stats_dump(void) {}

#endif // APTH_PROFILE_HOOKS

#endif // __LIBAPTH_INTERNAL_STATS_H
