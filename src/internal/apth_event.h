#ifndef __LIBAPTH_INTERNAL_APTH_EVENT_H
#define __LIBAPTH_INTERNAL_APTH_EVENT_H

#include "common.h"
#include "apth.h"
#include "internal/apth_sched.h"
#include "internal/apth_state.h"
#include "internal/apth_time.h"
#include "utils/archplattoold.h"
#include "utils/list.h"
#include <stdbool.h>
#include <stdint.h>

// Event status code
typedef enum
{
    APTH_EV_STATUS_PENDING,
    APTH_EV_STATUS_OCCURRED,
    APTH_EV_STATUS_FAILED,
} apth_ev_status_t;

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

// Waiting on Mutex (no fields needed; wakeup is handled directly by unlock)
struct apth_event_mutex_st
{
    char _pad; // C requires non-empty struct
};

// Waiting on Conditional variables (no fields needed; wakeup is handled directly by signal/broadcast)
struct apth_event_cond_st
{
    char _pad; // C requires non-empty struct
};

// Waiting on another thread
struct apth_event_tid_st
{
    apth_t tid;
};

typedef bool (*apth_event_custom_func_t)(void *);
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
    bool epoll_registered;
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
#define APTH_EVENT_NULL ((apth_event_t)NULL)

APTH_INTERNAL void apth_sched_eventmanager_epoll(apth_sched_t sched, apth_time_t *now, bool dopoll);
APTH_INTERNAL void apth_sched_process_pending_fd_closes(apth_sched_t sched);
APTH_INTERNAL void apth_event_list_add(struct list *el, apth_event_t ev);
APTH_INTERNAL void apth_event_isolate(apth_event_t ev);
APTH_INTERNAL int apth_wait_event_list(struct list *el);
APTH_INTERNAL bool apth_wait_event(apth_event_t ev);
APTH_INTERNAL apth_event_t apth_event_fd(unsigned long spec, int fd);
APTH_INTERNAL apth_event_t apth_event_select(unsigned long spec, int *n, int nfd,
                                             fd_set *rfds, fd_set *wfds, fd_set *efds);
APTH_INTERNAL apth_event_t apth_event_sigs(unsigned long spec, const sigset_t *sigs, int *sig);
APTH_INTERNAL apth_event_t apth_event_time(unsigned long spec, apth_time_t tv);
APTH_INTERNAL apth_event_t apth_event_tid(unsigned long spec, apth_t tid);
APTH_INTERNAL apth_event_t apth_event_func(unsigned long spec, apth_event_custom_func_t func,
                                           void *arg, apth_time_t tv);
APTH_INTERNAL bool apth_event_free(apth_event_t ev);
APTH_INTERNAL bool apth_state_matches_event_goal(apth_state_t state, apth_goal_t goal);

#endif // __LIBAPTH_INTERNAL_APTH_EVENT_H
