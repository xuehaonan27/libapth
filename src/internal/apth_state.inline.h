#ifndef __LIBAPTH_INTERNAL_APTH_STATE_INLINE_H
#define __LIBAPTH_INTERNAL_APTH_STATE_INLINE_H

#include "internal_types.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/archplattoold.h"
#include "internal/apth_worker.inline.h"
#include "internal/apth_thqueue.inline.h"

// NEW: Use current_sched from ownership system
INLINE apth_sched_t sched_of(apth_t th)
{
    return th->current_sched;
}

// NEW: Use current_queue from ownership system
INLINE apth_state_t queue_state_of(apth_t th)
{
    return th->current_queue->th_state;
}

#endif // __LIBAPTH_INTERNAL_APTH_STATE_INLINE_H
