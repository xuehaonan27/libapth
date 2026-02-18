#ifndef __LIBAPTH_INTERNAL_TYPES_H
#define __LIBAPTH_INTERNAL_TYPES_H

#define APTH_TCB_NAMELEN 31
#define _GNU_SOURCE // for pthread_attr_setaffinity_np
#include "apth.h"

#include "utils/list.h"
#include "utils/ring.h"
#include "utils/lll.h"
#include <sys/time.h>
#include <ucontext.h>
#include <pthread.h>

#define _BIT(n) (1 << (n))

extern _Atomic unsigned int WORKER_SPAWNED;
extern _Atomic unsigned int SYNC_BEFORE_MAIN_APTH_SPAWN;

extern _Atomic apth_t MAIN_APTH;
// extern _Atomic int MAIN_APTH_EXIT_BY_CALLING_APTH_EXIT;

// TODO: this is for debug only, remove it later
// extern _Atomic unsigned int SIMPLE_BARRIER;

// Event status code
typedef enum
{
    APTH_EV_STATUS_PENDING,
    APTH_EV_STATUS_OCCURRED,
    APTH_EV_STATUS_FAILED,
} apth_ev_status_t;

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

// ============================== Time ==============================
#define APTH_TIME_NOW (apth_time_t *)(0)
extern apth_time_t apth_time_zero;
#define APTH_TIME_ZERO &apth_time_zero

// ============================== Thread Data ==============================

struct apth_key_data
{
    uintptr_t seq; // Sequence number.
    void *data;    // Data pointer
};

struct apth_keytab_st
{
    // Sequence numbers. Even numbers indicated vacant entries,
    // Note that zero is even.
    _Atomic uintptr_t seq;
    // Destructor for the data.
    void (*destructor)(void *);
};

// Check whether an entry is unused.
#define APTH_KEY_UNUSED(p) (((p) & 1) == 0)

// Check whether a key is usable. We cannot reuse an allocated key if the
// sequence counter would overflow after the next destory call. This would
// mean that we potentially free memory for a key with the same sequence. This
// is very unlikely to happen, A program would have to create and destroy
// a key 2 ^ 31 (32-bits) or 2 ^ 63 (64-bits) times. If it should happen we
// simply don't use this specific key anymore.
#define APTH_KEY_USABLE(p) (((uintptr_t)(p)) < ((uintptr_t)((p) + 2)))

// ============================== Thread Cleanup ==============================

// Thread cleanup handlers
struct apth_cleanup_st
{
    struct apth_cleanup_st *next;
    void (*func)(void *);
    void *arg;
};
typedef struct apth_cleanup_st *apth_cleanup_t;

// ============================== Thread Scheduler ==============================
typedef int sched_id;

// We don't want to expose this struct to public space
typedef struct apth_perpthr_scheduler *apth_sched_t;

// We don't want to expose this struct to public space
typedef struct apth_worker_st *apth_worker_t;

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
    lll_t new_list_lock;         // synchronize access to new list
    lll_t ready_list_lock;       // synchronize access to ready list
    lll_t waiting_list_lock;     // synchronize access to waiting list
    lll_t suspended_list_lock;   // synchronize access to suspended list
    lll_t terminated_list_lock;  // synchronize access to terminated list
    apth_worker_t worker;        // pthread worker carrying this scheduler
    unsigned int switches;       // context switch times
    _Atomic unsigned int thrcnt; // APTH threads now running on this scheduler
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

// ============================== APTH TCB ==============================

// Thread control block.
struct ALIGNED(8) apth_st 
{
    /* standard thread control block ingredients */
    int prio;                    /* base priority of thread             */
    char name[APTH_TCB_NAMELEN + 1]; /* name of thread                      */
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
    // bool joinable;  /* whether thread is joinable                       */
    _Atomic apth_t joinid; /* Who is joining me? */
#define IS_DETACHED(pd) ((pd)->joinid == (pd))
    void *join_arg; /* joining argument                                 */

    /* cancellation support */
    bool cancelreq;                      /* cancellation request is pending        */
    _Atomic unsigned int cancelhandling; /* cancellation state of thread           */
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
#define APTH_CANCELED ((void *)-1)
    apth_cleanup_t cleanups; /* stack of thread cleanup handlers       */

    /* mutex ring */
    struct ring mutexring; /* ring of aquired mutex structures          */

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
    apth_worker_t worker; // TODO: when performing work stealing, modify this
    struct list *belongs_to_list;
    lll_t *belongs_to_list_lock;
};
#define APTH_NULL (apth_t) NULL
#define APTH_FAKE_SCHED(sched) ((apth_t)((uintptr_t)(sched) | 0x1))
#define APTH_IS_FAKE_SCHED(val) (((uintptr_t)(val) & 0x1) != 0)

// Default stack size by bytes
#define APTH_STACK_SIZE_DEFAULT 8192

// FIXME: this should be set by build system
// Indicate stack growth direction is from high to low (< 0) or from low to
// high (> 0). On most systems this should usually be the former.
#define APTH_STACKGROWTH (-1)
#define APTH_MAGIC 0xCAFEBABE

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
/* event occurange restrictions */
#define APTH_GOAL_UNTIL_OCCURRED _BIT(11)
#define APTH_GOAL_UNTIL_FD_READABLE _BIT(12)
#define APTH_GOAL_UNTIL_FD_WRITEABLE _BIT(13)
#define APTH_GOAL_UNTIL_FD_EXCEPTION _BIT(14)
#define APTH_GOAL_UNTIL_TID_NEW _BIT(15)
#define APTH_GOAL_UNTIL_TID_READY _BIT(16)
#define APTH_GOAL_UNTIL_TID_WAITING _BIT(17)
#define APTH_GOAL_UNTIL_TID_DEAD _BIT(18)

/* event structure handling modes */
#define APTH_EVENT_MODE_REUSE _BIT(20)
#define APTH_EVENT_MODE_CHAIN _BIT(21)
#define APTH_EVENT_MODE_STATIC _BIT(22)

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

// ============================== Thread Attr ==============================
// Thread attributes
struct apth_attr_st
{
    // Scheduler parameters and priority
    struct sched_param schedparam;
    int schedpolicy;
    // Various flags like detachstate, scope, etc
    int flags;
    // Size of guard area
    size_t guardsize;
    // Stack handling
    void *stackaddr;
    size_t stacksize;
    char name[APTH_TCB_NAMELEN + 1];

    /* These are extensions. Modified according to APTH needs */
    // Affinity map
    cpu_set_t *cpuset;
    size_t cpusetsize;
    sigset_t sigmask;
    bool sigmask_set;
};

#define ATTR_FLAG_DETACHSTATE 0x0001
#define ATTR_FLAG_NOTINHERITSCHED 0x0002
#define ATTR_FLAG_SCOPEPROCESS 0x0004
#define ATTR_FLAG_STACKADDR 0x0008
#define ATTR_FLAG_OLDATTR 0x0010
#define ATTR_FLAG_SCHED_SET 0x0020
#define ATTR_FLAG_POLICY_SET 0x0040
#define ATTR_FLAG_DO_RSEQ 0x0080

// ============================== Filedescriptors ==============================
// Filedescriptor blocking modes
enum
{
    APTH_FDMODE_ERROR = -1,
    APTH_FDMODE_POLL = 0,
    APTH_FDMODE_BLOCK,
    APTH_FDMODE_NONBLOCK
};
#include <fcntl.h>
// Non-blocking flags
#define APTH_O_NONBLOCKING O_NONBLOCK

#include <paths.h>
#define APTH_PATH_BINSH _PATH_BSHELL

#endif /* __LIBAPTH_INTERNAL_TYPES_H */
