#ifndef __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_EVENT_ST_H
#define __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_EVENT_ST_H

#include "apth.h"
#include "internal/forward_declare.h"
#include "internal/apth_time.h"
#include "utils/list.h"

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
    APTH_EVENT_TYPE_SYNC,
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
    const sigset_t *sigs;
    int *sig;
};

// Waiting on Time
struct apth_event_time_st
{
    apth_time_t tv;
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
        struct apth_event_tid_st TID;
        struct apth_event_func_st FUNC;
    } ev_args;
#define apth_event_t_list_entry(LIST_ELEM) \
    list_entry(LIST_ELEM, struct apth_event_st, elem)
};

/* event occurange restrictions */
#define APTH_GOAL_UNTIL_OCCURRED _BIT(11)
#define APTH_GOAL_UNTIL_FD_READABLE _BIT(12)
#define APTH_GOAL_UNTIL_FD_WRITEABLE _BIT(13)
#define APTH_GOAL_UNTIL_FD_EXCEPTION _BIT(14)
#define APTH_GOAL_UNTIL_TID_NEW _BIT(15)
#define APTH_GOAL_UNTIL_TID_READY _BIT(16)
#define APTH_GOAL_UNTIL_TID_WAITING _BIT(17)
#define APTH_GOAL_UNTIL_TID_DEAD _BIT(18)

#define APTH_EVENT_NULL ((apth_event_t)NULL)

#endif // __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_EVENT_ST_H
