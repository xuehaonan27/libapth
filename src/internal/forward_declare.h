#ifndef __LIBAPTH_INTERNAL_FORWARD_DECLARE_H
#define __LIBAPTH_INTERNAL_FORWARD_DECLARE_H

#include "utils/archplattoold.h"

typedef int sched_id;
typedef struct apth_perpthr_scheduler *apth_sched_t;
typedef struct apth_worker_st *apth_worker_t;
typedef struct apth_thqueue_st *apth_thqueue_t;

// NEW: Use current_sched from ownership system
// INLINE apth_sched_t sched_of(apth_t th)
// {
//     return th->current_sched;
// }

// INLINE_ALWAYS apth_t cur_apth(void)
// {
//     return cur_sched()->cur;
// }

// INLINE_ALWAYS void set_cur_apth(apth_t t)
// {
//     assert(t != (apth_t)NULL);
//     cur_sched()->cur = t;
// }

#endif // __LIBAPTH_INTERNAL_FORWARD_DECLARE_H
