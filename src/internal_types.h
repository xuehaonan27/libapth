#ifndef __LIBAPTH_INTERNAL_TYPES_H
#define __LIBAPTH_INTERNAL_TYPES_H

#define APTH_TCB_NAMELEN 16

#include "apth.h"

#include "utils/list.h"
#include "utils/ring.h"
#include <sys/time.h>
#include <ucontext.h>
#include <pthread.h>
// #include <stdatomic.h>

#define _BIT(n) (1 << (n))

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
    struct list event_list; /* events the tread is waiting for          */

    /* per-thread signal handling */
    sigset_t sigpending; /* set    of pending signals                   */
    int sigpendcnt;      /* number of pending signals                   */

    /* machine context */
    apth_cxt_t ctx;              /* last saved context of thread        */
    char *stack;                 /* pointer to thread stack             */
    size_t stacksize;            /* size of thread stack                */
    uint32_t *stackguard;        /* stack overflow guard                */
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
    bool cancelreq;           /* cancellation request is pending        */
    unsigned int cancelstate; /* cancellation state of thread           */
    apth_cleanup_t *cleanups; /* stack of thread cleanup handlers       */

    /* mutex ring */
    struct ring mutexring; /* ring of aquired mutex structures          */

    /* per-thread exception handling */
    // TODO: exception handling

    /* scheduler list handling */
    struct list_elem elem;
#define apth_t_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_st, elem)
};
#define APTH_NULL (apth_t) NULL

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

enum apth_event_type
{
    APTH_EVENT_TYPE_FD,
    APTH_EVENT_TYPE_SELECT,
    APTH_EVENT_TYPE_SIGS,
    APTH_EVENT_TYPE_TIME,
    APTH_EVENT_TYPE_MUTEX,
    APTH_EVENT_TYPE_COND,
    APTH_EVENT_TYPE_TID,
    APTH_EVENT_TYPE_FUNC
};

// Waiting on FD
struct apth_event_fd_st
{
    int fd;
};

// Waiting on Select FD I/O
struct apth_event_select_st
{
    int *n;
    int nfd;
    fd_set *rfds;
    fd_set *wfds;
    fd_set *efds;
};

// Waiting on signals
struct apth_event_sigs_st
{
    sigset_t *sigs;
    int *sig;
};

// Waiting on Time
struct apth_event_time_st
{
    apth_time_t tv;
};

// Waiting on Mutex
struct apth_event_mutex_st
{
    // TODO: mutex
};

// Waiting on Conditional variables
struct apth_event_cond_st
{
    // TODO: cond
};

// Waiting on another thread
struct apth_event_tid_st
{
    apth_t tid;
};

typedef int (*apth_event_custom_func_t)(void *);
// Waiting on custom functions
struct apth_event_func_st
{
    apth_event_custom_func_t func;
    void *arg;
    apth_time_t tv;
};

typedef int apth_goal_t;
#define APTH_GOAL_UNTIL_OCCURRED _BIT(0)
#define APTH_GOAL_UNTIL_FD_READABLE _BIT(1)
#define APTH_GOAL_UNTIL_FD_WRITEABLE _BIT(2)
#define APTH_GOAL_UNTIL_FD_EXCEPTION _BIT(3)
#define APTH_GOAL_UNTIL_TID_NEW _BIT(4)
#define APTH_GOAL_UNTIL_TID_READY _BIT(5)
#define APTH_GOAL_UNTIL_TID_WAITING _BIT(6)
#define APTH_GOAL_UNTIL_TID_DEAD _BIT(7)

// APTH Events
struct apth_event_st
{
    struct list_elem elem;
    apth_ev_status_t ev_status;
    enum apth_event_type ev_type;
    apth_goal_t ev_goal;
    union
    {
        struct apth_event_fd_st FD;
        struct apth_event_select_st SELECT;
        struct apth_event_sigs_st SIGS;
        struct apth_event_time_st TIME;
        struct apth_event_mutex_st MUTEX;
        struct apth_event_cond_st COND;
        struct apth_event_tid_st TID;
        struct apth_event_func_st FUNC;
    } ev_args;
#define apth_event_t_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_event_st, elem)
};
typedef struct apth_event_st *apth_event_t;
#define APTH_EVENT_NULL NULL

// ============================== Thread Context ==============================
// APTH Thread context
struct apth_cxt_st
{
    ucontext_t uc;
    sigset_t sigs;
    int error;
    bool restored;
};
typedef struct apth_cxt_st *apth_cxt_t;

// ============================== Thread Scheduler ==============================
typedef int sched_id;

#define APTH_NSIG 65

// Per-thread scheduler. Note that we do not treat scheduler as a separated
// thread but a background role. Besides, since the main thread only runs on
// one of schedulers, holding a special reference field to the main thread is
// meaningless here.
struct apth_perpthr_scheduler
{
    sched_id id;                 // scheduler ID
    apth_cxt_t sched_ctx;        // scheduler context (as trampoline)
    struct list new_list;        // new threads
    struct list ready_list;      // threads ready to run [elem: struct apth_st]
    struct list waiting_list;    // threads waiting for an event [elem: struct apth_st]
    struct list suspended_list;  // suspended threads [elem: struct apth_st]
    struct list terminated_list; // terminated threads [elem: struct apth_st]
    apth_worker_t worker;        // pthread worker carrying this scheduler
    unsigned int switches;       // context switch times
    unsigned int thrcnt;         // APTH threads now running on this scheduler
    apth_time_t running;         // time the scheduler runs
    apth_t cur;                  // current APTH
    _Atomic bool opening;        // scheduler is opening
    int apth_sigpipe[2];         // internal signal occurrence pipe
    sigset_t apth_sigpending;    // mask of pending signals
    sigset_t apth_sigblock;      // mask of signals we block in scheduler
    sigset_t apth_sigcatch;      // mask of signals we have to catch
    sigset_t apth_sigraised;     // mask of raised signals
    apth_time_t apth_loadticknext;
    float loadval;
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
    struct list_elem elem;
#define apth_worker_t_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_worker_st, elem)
};
// We don't want to expose this struct to public space
typedef struct apth_worker_st *apth_worker_t;

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
    int init_worker_count;                    // initially spawned workers
    struct apth_worker_st *workers_mem_start; // start memory address of init workers
    // struct apth_worker_t_list_elem *worker_elems_mem_start; // start memory address of init worker list elems
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

#define APTH_TIME_NOW (apth_time_t *)(0)
extern apth_time_t apth_time_zero;
#define APTH_TIME_ZERO &apth_time_zero

// ============================== Thread Data ==============================

struct apth_keytab_st
{
    // Sequence numbers. Even numbers indicated vacant entries,
    // Note that zero is even.
    uintptr_t seq;
    // Destructor for the data.
    void (*destructor)(void *);
};

// Check whether an entry is unused.
#define APTH_KEY_UNUSED(p) (((p) & 1) == 0)

// Check whether a key is usable.
#define APTH_KEY_USABLE(p) (((uintptr_t)(p)) < ((uintptr_t)((p) + 2)))

#endif /* __LIBAPTH_INTERNAL_TYPES_H */
