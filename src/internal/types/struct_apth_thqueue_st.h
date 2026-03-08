#ifndef __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_THQUEUE_ST_H
#define __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_THQUEUE_ST_H

#include "internal/forward_declare.h"
#include "internal/apth_tcb.h"
#include "internal/apth_state.h"
#include "utils/lll_new.h"
#include "utils/list.inline.h"

struct apth_thqueue_st
{
    struct list th_list;
    lll_internal_t th_list_lock;
    apth_state_t th_state; // State that apths belonging to this queue should be
    size_t size;           // Non-atomic, protected by lock
};

typedef void drain_thqueue_th_func(apth_t th);
// This return values from `visit_thqueue_th_func` indicates not an apth_thqueue,
// but just to tell `visit_thqueue`, take that count into its return value, but
// do not move `th`. Useful in fist loop of event manager.
#define APTH_DONT_MOVE_BUT_COUNT (apth_thqueue_t)(-1)
typedef apth_thqueue_t visit_thqueue_th_func(apth_t th, void *);
typedef bool find_first_in_thqueue_th_func(apth_t th, void *);

#endif // __LIBAPTH_INTERNAL_TYPES_STRUCT_APTH_THQUEUE_ST_H
