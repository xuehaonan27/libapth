#ifndef __LIBAPTH_INTERNAL_APTH_CLEANUP_H
#define __LIBAPTH_INTERNAL_APTH_CLEANUP_H

#include "apth.h"
#include "utils/archplattoold.h"

// Thread cleanup handlers
struct apth_cleanup_st
{
    struct apth_cleanup_st *next;
    void (*func)(void *);
    void *arg;
};
typedef struct apth_cleanup_st *apth_cleanup_t;

APTH_INTERNAL void apth_thread_cleanup(apth_t th);

#endif // __LIBAPTH_INTERNAL_APTH_CLEANUP_H
