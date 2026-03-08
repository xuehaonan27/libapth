#ifndef __LIBAPTH_INTERNAL_APTH_EVENT_H
#define __LIBAPTH_INTERNAL_APTH_EVENT_H

#include "common.h"
#include "internal/types/struct_apth_event_st.h"
#include "internal/apth_state.h"
#include <stdbool.h>

APTH_INTERNAL void apth_sched_eventmanager_epoll(apth_sched_t sched, apth_time_t *now, bool dopoll);
APTH_INTERNAL void apth_sched_process_pending_fd_closes(apth_sched_t sched);
APTH_INTERNAL void apth_event_list_add(struct list *el, apth_event_t ev);
APTH_INTERNAL void apth_event_isolate(apth_event_t ev);
APTH_INTERNAL int apth_wait_event_list(struct list *el);
APTH_INTERNAL bool apth_wait_event(apth_event_t ev);
APTH_INTERNAL apth_event_t apth_event_select(unsigned long spec, int *n, int nfd,
                                             fd_set *rfds, fd_set *wfds, fd_set *efds);
APTH_INTERNAL apth_event_t apth_event_tid(unsigned long spec, apth_t tid);
APTH_INTERNAL apth_event_t apth_event_func(unsigned long spec, apth_event_custom_func_t func,
                                           void *arg, apth_time_t tv);
APTH_INTERNAL bool apth_event_free(apth_event_t ev);
APTH_INTERNAL bool apth_state_matches_event_goal(apth_state_t state, apth_goal_t goal);

#define EVENT_FD(ARG_FD, ARG_GOAL)                                  \
    {.elem = {0},                                                   \
     .ev_status = APTH_EV_STATUS_PENDING,                           \
     .ev_type = APTH_EVENT_TYPE_FD,                                 \
     .ev_goal = (ARG_GOAL),                                         \
     .epoll_registered = false,                                     \
     .ev_args.FD.fd = (ARG_FD)};                                    \
    do                                                              \
    {                                                               \
        if (((ARG_GOAL) &                                           \
             (APTH_GOAL_UNTIL_FD_READABLE |                         \
              APTH_GOAL_UNTIL_FD_WRITEABLE |                        \
              APTH_GOAL_UNTIL_FD_EXCEPTION)) != (ARG_GOAL))         \
            PANIC("Goal of FD event wrong = " stringify(ARG_GOAL)); \
    } while (0)

#define EVENT_SIGS(ARG_SIGS, ARG_SIG)     \
    {.elem = {0},                         \
     .ev_status = APTH_EV_STATUS_PENDING, \
     .ev_type = APTH_EVENT_TYPE_SIGS,     \
     .ev_goal = 0,                        \
     .epoll_registered = false,           \
     .ev_args.SIGS.sigs = (ARG_SIGS),     \
     .ev_args.SIGS.sig = (ARG_SIG)}

#define EVENT_TIME(TV)                    \
    {.elem = {0},                         \
     .ev_status = APTH_EV_STATUS_PENDING, \
     .ev_type = APTH_EVENT_TYPE_TIME,     \
     .ev_goal = APTH_GOAL_UNTIL_OCCURRED, \
     .epoll_registered = false,           \
     .ev_args.TIME.tv = (TV)}

#endif // __LIBAPTH_INTERNAL_APTH_EVENT_H
