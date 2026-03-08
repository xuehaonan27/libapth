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
