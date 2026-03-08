#ifndef __LIBAPTH_INTERNAL_APTH_STATE_H
#define __LIBAPTH_INTERNAL_APTH_STATE_H

#include "common.h"

// Thread state
typedef enum
{
    APTH_STATE_NEW = _BIT(1),        /* spawned, but still not dispatched */
    APTH_STATE_READY = _BIT(2),      /* ready, waiting to be dispatched   */
    APTH_STATE_WAITING = _BIT(3),    /* waiting until event occurred      */
    APTH_STATE_TERMINATED = _BIT(4), /* terminated, waiting to be joined  */
    APTH_STATE_WAKED = _BIT(5),      /* waked, moving back to ready queue */
    APTH_STATE_RUNNING = _BIT(6)     /* running, should only have 1 element */
} apth_state_t;

#endif // __LIBAPTH_INTERNAL_APTH_STATE_H
