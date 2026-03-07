#ifndef __LIBAPTH_INTERNAL_APTH_STATE_H
#define __LIBAPTH_INTERNAL_APTH_STATE_H

// Thread state
typedef enum
{
    APTH_STATE_NEW = 2,         /* spawned, but still not dispatched */
    APTH_STATE_READY = 4,       /* ready, waiting to be dispatched   */
    APTH_STATE_WAITING = 8,     /* waiting until event occurred      */
    APTH_STATE_TERMINATED = 16, /* terminated, waiting to be joined  */
    APTH_STATE_WAKED = 32,
    APTH_STATE_RUNNING = 64
} apth_state_t;

#endif // __LIBAPTH_INTERNAL_APTH_STATE_H
