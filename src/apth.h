#ifndef __LIBAPTH_H
#define __LIBAPTH_H

#include <stdbool.h>

// Initialize and destruct.
// int apth_init(void) __attribute__((constructor));
// int apth_drop(void) __attribute__((destructor));

int apth_init(void);
int apth_drop(void);

// ==================== INCLUDE SYS HEADERS ====================
#include <bits/types/struct_timeval.h>
typedef struct timeval apth_time_t;

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

// ==================== Thread Attributes ====================

struct apth_attr_st;
typedef struct apth_attr_st apth_attr_t;

typedef unsigned int apth_key_t;

// ==================== Cleanup ====================
enum
{
    APTH_CANCEL_ENABLE = 0,
#define APTH_CANCEL_ENABLE APTH_CANCEL_ENABLE
    APTH_CANCEL_DISABLE
#define APTH_CANCEL_DISABLE APTH_CANCEL_DISABLE
};
enum
{
    APTH_CANCEL_DEFERRED = 0,
#define APTH_CANCEL_DEFERRED APTH_CANCEL_DEFERRED
    APTH_CANCEL_ASYNCHRONOUS
#define APTH_CANCEL_ASYNCHRONOUS APTH_CANCEL_ASYNCHRONOUS
};

// ==================== Functions ====================
int apth_cancel(apth_t th);
bool apth_cleanup_push(void (*func)(void *), void *arg);
bool apth_cleanup_pop(int execute);
int apth_create(apth_t *newthr, const apth_attr_t *attr,
                void *(*start_routine)(void *), void *__arg);
int apth_key_create(apth_key_t *key, void (*destr)(void *));
int apth_key_delete(apth_key_t key);
void *apth_getspecific(apth_key_t key);
int apth_setspecific(apth_key_t key, const void *value);
int apth_detach(apth_t th);
void apth_exit(void *retval);
int apth_join(apth_t tid, void **value);
apth_t apth_self(void);
int apth_setcancelstate(int state, int *oldstate);
int apth_setcanceltype(int type, int *oldtype);
int apth_setname_np(apth_t th, const char *name);
int apth_yield(void);
int apth_kill(apth_t t, int sig);
int apth_equal(apth_t t1, apth_t t2);

#endif /* __LIBAPTH_H */
