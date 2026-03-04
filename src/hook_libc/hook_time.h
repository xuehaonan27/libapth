#ifndef __LIBAPTH_HOOK_LIBC_HOOK_TIME_H
#define __LIBAPTH_HOOK_LIBC_HOOK_TIME_H

#include "hook_utils.h"
#include <bits/types/struct_timespec.h>

#define APTH_LIST_OF_HOOK_TIME \
    X(nanosleep)               \
    X(usleep)                  \
    X(sleep)

APTH_DECLARE_HOOK(int, nanosleep, const struct timespec *rqtp, struct timespec *rmtp)
APTH_DECLARE_HOOK(int, usleep, unsigned int usec)
APTH_DECLARE_HOOK(unsigned int, sleep, unsigned int sec)

#endif // __LIBAPTH_HOOK_LIBC_HOOK_TIME_H
