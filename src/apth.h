#ifndef __LIBAPTH_H
#define __LIBAPTH_H

// Thread identifier
typedef struct apth_st *apth_t;
struct apth_st;

// Thread state
typedef enum
{
    APTH_STATE_SCHEDULER = 0, /* the special scheduler thread only       */
    APTH_STATE_NEW,           /* spawned, but still not dispatched       */
    APTH_STATE_READY,         /* ready, waiting to be dispatched         */
    APTH_STATE_WAITING,       /* suspended, waiting until event occurred */
    APTH_STATE_TERMINATED,    /* terminated, waiting to be joined        */
} apth_state_t;

// Event status code
typedef enum
{
    APTH_EV_STATUS_PENDING,
    APTH_EV_STATUS_OCCURRED,
    APTH_EV_STATUS_FAILED,
} apth_ev_status_t;

struct apth_attr_st;
typedef struct apth_attr_st apth_attr_t;

typedef unsigned int apth_key_t;

// ==================== INCLUDE SYS HEADERS ====================
#include <bits/types/struct_timeval.h>
typedef struct timeval apth_time_t;

#endif /* __LIBAPTH_H */
