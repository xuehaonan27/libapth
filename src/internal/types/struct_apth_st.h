#ifndef __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_ST_H
#define __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_ST_H

#include "common.h" // For APTH_TCB_NAMELEN
#include "apth.h"
#include "internal/forward_declare.h"
#include "internal/types/struct_apth_event_st.h"
#include "internal/apth_epoll_waiter.h"
#include "internal/apth_cleanup.h"
#include "internal/apth_ctx.h"
#include "internal/apth_data.h"
#include "internal/apth_state.h"
#include "internal/apth_time.h"
#include "utils/archplattoold.h"
#include "utils/lll.h"
#include "utils/list.h"
#include <stdint.h>
#include <stdbool.h>

typedef uintptr_t apth_yield_reason_t;

// Thread control block.
//
// Fields are organized into HOT / WARM / COLD sections for cache
// efficiency.  HOT fields are touched on every context switch and
// scheduling decision; they should fit within the first 1-2 cache lines.
// WARM fields are accessed during I/O waits and synchronization.
// COLD fields are large or rarely accessed and are kept at the tail.
struct ALIGNED(64) apth_st
{
    // ==================== HOT: touched every context switch ====================
    _Atomic(apth_state_t) state;          // 4B  - thread state
    apth_yield_reason_t yield_reason;     // 8B  - why the APTH yields
    struct list_elem elem;                // 16B - intrusive list linkage
#define apth_t_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_st, elem)
    apth_sched_t current_sched;           // 8B  - which scheduler currently owns this APTH
    apth_thqueue_t current_queue;         // 8B  - which queue this APTH is on
    _Atomic(apth_t) joinid;              // 8B  - APTH that's joining us
#define IS_DETACHED(pd) ((pd)->joinid == (pd))
    _Atomic(bool) cancelreq;             // 1B  - cancellation request pending
    bool wake_pending;                    // 1B  - set by epoll_map_wake_fd
    uint32_t magic;                       // 4B  - validation magic number
    int dispatches;                       // 4B  - total dispatches
    int thread_class;                     // 4B  - APTH_CLASS_DEFAULT/IO_BOUND/CPU_BOUND/REALTIME
    // ~62 bytes up to here — fits in one 64-byte cache line

    // ==================== WARM: touched during I/O/sync ====================
    struct list event_list;               // events the thread is waiting for
    int sigpendcnt;                       // number of pending signals
    _Atomic(unsigned int) cancelhandling; // cancellation state/type
#define CANCELSTATE_BITMASK _BIT(0)
#define CANCELTYPE_BITMASK _BIT(1)
    apth_cleanup_t cleanups;              // stack of thread cleanup handlers
    int prio;                             // base priority of thread
    char name[APTH_TCB_NAMELEN + 1];      // name of thread

    /* Time statistics */
    apth_time_t spawned;
    apth_time_t lastran;
    apth_time_t running;
    uint64_t last_yield_tick;
    int yield_timeslice;

    /* Thread Joining */
    void *join_arg;

    /* Stack */
    char *stack_mem_start;
    size_t stacksize;
    size_t guardsize;
    bool stackloan;
    void *(*start_func)(void *);
    void *start_arg;

    /* Ownership */
    apth_sched_t home_sched;              // immutable: set at creation
    lll_internal_t ownership_lock;        // cross-scheduler operations lock

    /* Dedicated thread (hybrid scheduling): 1:1 pthread mode */
    bool is_dedicated;                    // true if this is a 1:1 dedicated pthread
    pthread_t dedicated_tid;              // backing pthread (only if is_dedicated)
    int dedicated_wake_fd;                // eventfd for blocking (only if is_dedicated)
    apth_sched_t dedicated_dummy_sched;   // dummy scheduler for TLS (only if is_dedicated)
    struct list_elem dedicated_elem;      // linkage for dedicated thread registry

    // ==================== COLD: rarely accessed / large ====================

    /* Per APTH Signal Handling */
    sigset_t sigpending;                  // 128B on Linux
    sigset_t sigmask;                     // 128B on Linux
    lll_internal_t siglock;
    stack_t signalstack;
    bool sigaltstack_set;
    volatile bool in_sighandler;

    /* Context — lightweight: saved stack pointer + errno (16B).
     * Callee-saved registers are stored on the thread's own stack by
     * the assembly context-switch routine (apth_ctx_x86_64.S). */
    struct apth_cxt_st ctx_st;
#define CTX(T) ((apth_cxt_t) & ((T)->ctx_st))

    /* Thread Specific Data (TLS) */
#define APTH_KEYS_MAX 1024
#define APTH_KEY_2NDLEVEL_SIZE 32
#define APTH_KEY_1STLEVEL_SIZE \
    ((APTH_KEYS_MAX + APTH_KEY_2NDLEVEL_SIZE - 1) / APTH_KEY_2NDLEVEL_SIZE)
    struct apth_key_data specific_1stblock[APTH_KEY_2NDLEVEL_SIZE];
    struct apth_key_data *specific[APTH_KEY_1STLEVEL_SIZE];
    bool specific_used;
};

#define APTH_NULL ((apth_t)NULL)

#define APTH_MAGIC 0xCAFEBABE
#define APTH_TH_MAGIC_IS_GOOD(th) ((th)->magic == APTH_MAGIC)

// Apth is with a valid tid. Note that even apth is terminated, its tid
// is still valid, until `apth_tcb_free` reaps it.
#define APTH_IS_VALID(t) (((t) != APTH_NULL) && APTH_TH_MAGIC_IS_GOOD(t))

#endif // __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_ST_H
