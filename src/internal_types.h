#ifndef __LIBAPTH_INTERNAL_TYPES_H
#define __LIBAPTH_INTERNAL_TYPES_H

#define APTH_TCB_NAMELEN 40

#include "apth.h"

#include "utils/list.h"
#include "utils/ring.h"
#include <sys/time.h>
#include <ucontext.h>
#include <pthread.h>
// #include <stdatomic.h>

// ============================== APTH TCB ==============================

// Thread control block.
struct apth_st
{
    /* priority queue handling */
    apth_t q_next;
    apth_t q_prev;

    /* standard thread control block ingredients */
    int prio;                    /* base priority of thread             */
    char name[APTH_TCB_NAMELEN]; /* name of thread                      */
    int dispatches;              /* total number of thread dispatches   */
    apth_state_t state;          /* current state indicator for thread  */

    /* timing */
    apth_time_t spawned; /* time point at which thread was spawned      */
    apth_time_t lastran; /* time point at which thread was last running */
    apth_time_t running; /* time range the thread was already running   */

    /* event handling */
    apth_event_t events; /* events the tread is waiting for             */

    /* per-thread signal handling */
    sigset_t sigpending; /* set    of pending signals                   */
    int sigpendcnt;      /* number of pending signals                   */

    /* machine context */
    apth_cxt_t ctx;              /* last saved context of thread        */
    char *stack;                 /* pointer to thread stack             */
    size_t stacksize;            /* size of thread stack                */
    char *stackguard;            /* stack overflow guard                */
    bool stackloan;              /* stack type                          */
    void *(*start_func)(void *); /* start routine                       */
    void *start_arg;             /* start argument                      */

    /* thread joining */
    bool joinable;  /* whether thread is joinable                       */
    void *join_arg; /* joining argument                                 */

    /* per-thread specific storage */
    const void **data_value; /* thread specific  values                 */
    int data_count;          /* number of stored values                 */

    /* cancellation support */
    int cancelreq;            /* cancellation request is pending        */
    unsigned int cancelstate; /* cancellation state of thread           */
    apth_cleanup_t *cleanups; /* stack of thread cleanup handlers       */

    /* mutex ring */
    struct ring mutexring; /* ring of aquired mutex structures          */

    /* per-thread exception handling */
    // TODO: exception handling
};

struct apth_t_list_elem
{
    struct list_elem elem;
    apth_t ptcb;
#define apth_t_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_t_list_elem, elem)
};

// Default stack size by bytes
#define APTH_STACK_SIZE_DEFAULT 8192

// FIXME: this should be set by build system
// Indicate stack growth direction is from high to low (< 0) or from low to 
// high (> 0). On most systems this should usually be the former.
#define APTH_STACKGROWTH (-1)
#define APTH_MAGIC 0xCAFEBABE

const char *pth_state_names[] = {
    "scheduler",
    "new",
    "ready",
    "running",
    "waiting",
    "dead",
};

// ============================== Thread Events ==============================

// APTH Events
struct apth_event_st
{
    struct apth_event_st *ev_next;
    struct apth_event_st *ev_prev;
    apth_ev_status_t ev_status;
    // TODO: apth event
};
typedef struct apth_event_st *apth_event_t;

// ============================== Thread Context ==============================
// APTH Thread context
struct apth_cxt_st
{
    ucontext_t uc;
    int error;
    bool restored;
};
typedef struct apth_cxt_st *apth_cxt_t;

// ============================== Thread Scheduler ==============================
typedef int sched_id;

// Per-thread scheduler. Note that we do not treat scheduler as a separated
// thread but a background role. Besides, since the main thread only runs on
// one of schedulers, holding a special reference field to the main thread is
// meaningless here.
struct apth_perpthr_scheduler
{
    sched_id id;                 // scheduler ID
    apth_cxt_t sched_ctx;        // scheduler context (as trampoline)
    struct list new_list;        // new threads
    struct list ready_list;      // threads ready to run [elem: struct apth_t_list_elem]
    struct list waiting_list;    // threads waiting for an event [elem: struct apth_t_list_elem]
    struct list suspended_list;  // suspended threads [elem: struct apth_t_list_elem]
    struct list terminated_list; // terminated threads [elem: struct apth_t_list_elem]
    apth_worker_t worker;        // pthread worker carrying this scheduler
    unsigned int switches;       // context switch times
    unsigned int thrcnt;         // APTH threads now running on this scheduler
    apth_t running;              // current running APTH
};
// We don't want to expose this struct to public space
typedef struct apth_perpthr_scheduler *apth_sched_t;

// ============================== Worker ==============================

// Pthread worker occupying CPU and carrying APTH loads
struct apth_worker_st
{
    int worker_id;       // worker ID
    pthread_t tid;       // a worker pthread
    pthread_attr_t attr; // Worker pthread attribute
    apth_sched_t sched;  // Hold scheduler
};
// We don't want to expose this struct to public space
typedef struct apth_worker_st *apth_worker_t;
struct apth_worker_t_list_elem
{
    struct list_elem elem;
    apth_worker_t pworker;
#define apth_worker_t_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_worker_t_list_elem, elem)
};
// Argument passed to worker's pthread
struct apth_worker_pthread_arg
{
    apth_worker_t self; // pointer to this worker
};
typedef struct apth_worker_pthread_arg *apth_worker_arg_t;

// The whole process shares this pool. This pool should be placed in heap.
// Accessing to this pool should be synchronized.
struct apth_global_scheduler_pool
{
    // TODO: there should be a lock protecting access to this pool.
    // TODO: this lock should be RW-lock since there's a lot of reads and a few writes.
    // TODO: and at Pthread level.
    int worker_count;          // total worker pthreads count
    struct list wrkpthrs_list; // worker pthreads [elem: struct apth_worker_t_list_elem]

    /* Immutable fields */
    int init_worker_count;                                  // initially spawned workers
    struct apth_worker_st *workers_mem_start;               // start memory address of init workers
    struct apth_worker_t_list_elem *worker_elems_mem_start; // start memory address of init worker list elems
};

// ============================== Thread Cleanup ==============================

// Thread cleanup handlers
struct apth_cleanup_st
{
    apth_cleanup_t next;
    void (*func)(void *);
    void *arg;
};
typedef struct apth_cleanup_st *apth_cleanup_t;

// Thread attributes
struct apth_attr_st
{
    apth_t a_tid;
    int a_prio;
    int a_dispatches;
    char a_name[APTH_TCB_NAMELEN];
    bool a_joinable;
    unsigned int a_cancelstate;
    size_t a_stacksize;
    char *a_stackaddr;
};

#endif /* __LIBAPTH_INTERNAL_TYPES_H */
