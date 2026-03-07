#ifndef __LIBAPTH_INTERNAL_APTH_TCB_H
#define __LIBAPTH_INTERNAL_APTH_TCB_H

#include "common.h" // For APTH_TCB_NAMELEN
#include "apth.h"
#include "internal/forward_declare.h"
#include "internal/apth_cleanup.h"
#include "internal/apth_ctx.h"
#include "internal/apth_data.h"
#include "internal/apth_event.h"
// #include "internal/apth_sched.h"
#include "internal/apth_state.h"
// #include "internal/apth_thqueue.h"
#include "internal/apth_time.h"
#include "utils/archplattoold.h"
#include "utils/lll_new.h"
#include <stdint.h>
#include <stdbool.h>

typedef uintptr_t apth_yield_reason_t;
#define APTH_YIELD_REASON_VOLUNTEER ((uintptr_t)0)
#define APTH_YIELD_REASON_WAIT ((uintptr_t)0x2)
#define APTH_YIELD_REASON_TIMESLICE ((uintptr_t)0x4)
#define APTH_YIELD_REASON_EXIT ((uintptr_t)0x8)

// Thread control block.
struct ALIGNED(8) apth_st
{
    /* standard thread control block ingredients */
    int prio;                        // base priority of thread
    char name[APTH_TCB_NAMELEN + 1]; // name of thread
    int dispatches;                  // total number of thread dispatches

    /* NEW: Simplified state management */
    _Atomic(apth_state_t) state; // Simple atomic state

    /* timing */
    apth_time_t spawned; // time point at which thread was spawned
    apth_time_t lastran; // time point at which thread was last running
    apth_time_t running; // time range the thread was already running

    /* event handling */
    struct list event_list; // events the tread is waiting for

    // Embedded event structures to avoid malloc/free in common case
    // Most apths wait on at most 1-2 events at a time (e.g., one FD read/write)
    // For select/poll with many fds, we fall back to malloc
    struct apth_event_st embedded_event_1;
    struct apth_event_st embedded_event_2;
    bool embedded_event_1_in_use;
    bool embedded_event_2_in_use;

    /* per-thread signal handling */
    sigset_t sigpending;         // set of pending signals
    int sigpendcnt;              // number of pending signals
    sigset_t sigmask;            // signal mask of this apth
    lll_internal_t siglock;      // NEW: Type 2 LLL for signal handling synchronization
    stack_t signalstack;         // stack for signal handling
    bool sigaltstack_set;        // whether signalstack is set
    volatile bool in_sighandler; // whether we are now executing signal handler in signal stack

    /* machine context */
    apth_cxt_t ctx;              // last saved context of thread
    char *stack_mem_start;       // pointer to thread stack memory (including guard page if any)
    size_t stacksize;            // size of thread stack (excluding guard page)
    size_t guardsize;            // size of guard page
    uint32_t magic;              // magic number for validation (replaces stackguard)
    bool stackloan;              // stack type
    void *(*start_func)(void *); // start routine
    void *start_arg;             // start argument

    /* thread joining */
    _Atomic(apth_t) joinid; // Who is joining me? Only one apth can do this, store here
    // When (pd)->joinid == (pd), then `pd` is marked as DETACHED
#define IS_DETACHED(pd) ((pd)->joinid == (pd))
    void *join_arg; // joining argument

    /* cancellation support */
    _Atomic(bool) cancelreq;              // cancellation request is pending
    _Atomic(unsigned int) cancelhandling; // cancellation state of thread

    // Bit set if cancellation is disabled
#define CANCELSTATE_BITMASK _BIT(0)
    // Bit set if asynchronous cancellation mode is selected
#define CANCELTYPE_BITMASK _BIT(1)
    //     // Bit set if canceling has been initiated
    // #define CANCELING_BITMASK _BIT(2)
    //     // Bit set if canceled
    // #define CANCELED_BITMASK _BIT(3)
    //     // Bit set if thread is exiting
    // #define EXITING_BITMASK _BIT(4)
    //     // Bit set if thread terminated and TCB is freed
    // #define TERMINATED_BITMASK _BIT(5)
    //     // Bit set if thread is supposed to change XID
    // #define SETXID_BITMASK _BIT(6)

    apth_cleanup_t cleanups; // stack of thread cleanup handlers

    /* per-thread exception handling */
    // TODO: exception handling

    /* Thread specific data */
#define APTH_KEYS_MAX 1024

// We keep thread specific data in a special data structure, a two-level
// array. The top-level array contains pointers to dynamically allocated
// arrays of a certain number of data pointers. So we can implement a
// sparse array. Each dynmaic second-level array has APTH_KEY_2NDLEVEL_SIZE
// entries. This value should not be too large.
#define APTH_KEY_2NDLEVEL_SIZE 32

#define APTH_KEY_1STLEVEL_SIZE \
    ((APTH_KEYS_MAX + APTH_KEY_2NDLEVEL_SIZE - 1) / APTH_KEY_2NDLEVEL_SIZE)

    // We allocate one block of references here. This should be enough to avoid
    // allocating any memory dynamically for most applications
    struct apth_key_data specific_1stblock[APTH_KEY_2NDLEVEL_SIZE];
    struct apth_key_data *specific[APTH_KEY_1STLEVEL_SIZE];

    // Flag which is set when specific data is set.
    bool specific_used;

    /* scheduler list handling */
    struct list_elem elem;
#define apth_t_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_st, elem)

    _Atomic(apth_thqueue_t) belongs_to_queue;

    /* NEW: Ownership system for simplified state management */
    apth_sched_t home_sched;       // Immutable: set at creation, for work stealing decisions
    apth_sched_t current_sched;    // Mutable: which scheduler currently owns this APTH
    apth_thqueue_t current_queue;  // Which queue (protected by queue lock)
    lll_internal_t ownership_lock; // Type 2 LLL for cross-scheduler operations (stealing, join)

    uint64_t last_yield_tick;
    int yield_timeslice;
    apth_yield_reason_t yield_reason;
};

#define APTH_NULL ((apth_t)NULL)

#define APTH_MAGIC 0xCAFEBABE
#define APTH_TH_MAGIC_IS_GOOD(th) ((th)->magic == APTH_MAGIC)

// Apth is with a valid tid. Note that even apth is terminated, its tid
// is still valid, until `apth_tcb_free` reaps it.
#define APTH_IS_VALID(t) (((t) != APTH_NULL) && APTH_TH_MAGIC_IS_GOOD(t))

APTH_INTERNAL apth_t apth_tcb_alloc(size_t stacksize, void *stackaddr, size_t guardsize);
APTH_INTERNAL void apth_tcb_free(apth_t t);
APTH_INTERNAL char *apth_tcb_get_usable_stack_start(apth_t t);

#endif // __LIBAPTH_INTERNAL_APTH_CTX_H
