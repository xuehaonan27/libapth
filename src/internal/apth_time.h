#ifndef __LIBAPTH_INTERNAL_APTH_TIME_H
#define __LIBAPTH_INTERNAL_APTH_TIME_H

#include "utils/archplattoold.h"
#include <stdint.h>
#include <bits/types/struct_timeval.h>
typedef struct timeval apth_time_t;

extern apth_time_t apth_time_zero;

#define APTH_TIME_NOW (apth_time_t *)(0)
#define APTH_TIME_ZERO &apth_time_zero
#define APTH_TIME(sec, usec) {sec, usec}

APTH_INTERNAL uint64_t cpu_tick();
APTH_INTERNAL apth_time_t apth_time(long sec, long usec);
APTH_INTERNAL apth_time_t apth_timeout(long sec, long usec);
APTH_INTERNAL void apth_time_set(apth_time_t *t1, apth_time_t *t2);
APTH_INTERNAL void apth_time_add(apth_time_t *t1, apth_time_t *t2);
APTH_INTERNAL void apth_time_sub(apth_time_t *t1, apth_time_t *t2);
APTH_INTERNAL int apth_time_cmp(apth_time_t *t1, apth_time_t *t2);
APTH_INTERNAL double apth_time_t2d(apth_time_t *t);

#endif // __LIBAPTH_INTERNAL_APTH_TIME_H
