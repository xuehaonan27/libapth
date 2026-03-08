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
struct ALIGNED(8) apth_st
{
    /* Common TCB Ingredients */
    int prio;                        // base priority of thread
    int dispatches;                  // total number of thread dispatches
    char name[APTH_TCB_NAMELEN + 1]; // name of thread
    _Atomic(apth_state_t) state;     // Simple atomic state

    /* Time statistics */
    apth_time_t spawned; // time point at which thread was spawned
    apth_time_t lastran; // time point at which thread was last running
    apth_time_t running; // time range the thread was already running

    /* Events waiting on */
    struct list event_list; // events the tread is waiting for

    /* Per APTH Signal Handling */
    sigset_t sigpending;         // set of pending signals
    int sigpendcnt;              // number of pending signals
    sigset_t sigmask;            // signal mask of this apth
    lll_internal_t siglock;      // signal handling synchronization
    stack_t signalstack;         // stack for signal handling
    bool sigaltstack_set;        // whether signalstack is set
    volatile bool in_sighandler; // whether we are now executing signal handler in signal stack

    /* Context */
    uint64_t last_yield_tick;         // when did APTH yield last time
    int yield_timeslice;              // count of timeslices between each two of volunteer yielding
    apth_yield_reason_t yield_reason; // reason why the APTH yields
    struct apth_cxt_st ctx_st;        // saved context of thread
// Useful macro for accessing `ctx_st` neatly
#define CTX(T) ((apth_cxt_t) & ((T)->ctx_st))

    /* Stack */
    char *stack_mem_start;       // pointer to thread stack memory (including guard page if any)
    size_t stacksize;            // size of thread stack (excluding guard page)
    size_t guardsize;            // size of guard page
    uint32_t magic;              // magic number for validation (replaces stackguard)
    bool stackloan;              // stack type
    void *(*start_func)(void *); // start routine
    void *start_arg;             // start argument

    /* Thread Joining */
    void *join_arg;         // Result of this APTH
    _Atomic(apth_t) joinid; // APTH that's joining us. Only one APTH can do this, store here
// When (pd)->joinid == (pd), then `pd` is marked as DETACHED
#define IS_DETACHED(pd) ((pd)->joinid == (pd))

    /* Cancellation */
    _Atomic(bool) cancelreq;              // cancellation request is pending
    _Atomic(unsigned int) cancelhandling; // cancellation state of thread
// Bit set if cancellation is disabled
#define CANCELSTATE_BITMASK _BIT(0)
// Bit set if asynchronous cancellation mode is selected
#define CANCELTYPE_BITMASK _BIT(1)

    /* Cleanups */
    apth_cleanup_t cleanups; // stack of thread cleanup handlers

    /* Exception Handling */
    // TODO: exception handling

    /* Thread Specific Data (TLS) */
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
    bool specific_used; // flag which is set when specific data is set.

    /* APTH Ownership */
    struct list_elem elem;
#define apth_t_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_st, elem)
    // _Atomic(apth_thqueue_t) belongs_to_queue;
    apth_sched_t home_sched;       // immutable: set at creation, for work stealing decisions
    apth_sched_t current_sched;    // mutable: which scheduler currently owns this APTH
    apth_thqueue_t current_queue;  // which queue this APTH is on (protected by queue lock)
    lll_internal_t ownership_lock; // synchronize cross-scheduler operations (stealing, join)
};

#define APTH_NULL ((apth_t)NULL)

#define APTH_MAGIC 0xCAFEBABE
#define APTH_TH_MAGIC_IS_GOOD(th) ((th)->magic == APTH_MAGIC)

// Apth is with a valid tid. Note that even apth is terminated, its tid
// is still valid, until `apth_tcb_free` reaps it.
#define APTH_IS_VALID(t) (((t) != APTH_NULL) && APTH_TH_MAGIC_IS_GOOD(t))

#endif // __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_ST_H
