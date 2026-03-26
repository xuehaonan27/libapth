#ifndef __LIBAPTH_ATTR_APTH_ATTR_H
#define __LIBAPTH_ATTR_APTH_ATTR_H

#include "common.h"
#include "apth.h"
#include "utils/archplattoold.h"
#include <bits/types/struct_sched_param.h>
#include <stddef.h>
#include <stdbool.h>
#include <sched.h>  // For cpu_set_t
#include <signal.h> // For sigset_t
#include <errno.h>

// Thread attributes
struct apth_attr_st
{
    struct sched_param schedparam; // Scheduler parameters and priority (not used)
    int schedpolicy;               // Scheduler policy (not used)
    int flags;                     // Various flags like detachstate, scope, etc
    size_t guardsize;              // Size of guard area
    void *stackaddr;               // Stack address (NOT the start memory address of stack area)
    size_t stacksize;              // Stack size
    char name[APTH_TCB_NAMELEN + 1];

    /* These are extensions. Modified according to APTH needs */
    cpu_set_t *cpuset; // Affinity map
    size_t cpusetsize; // Size of affinity map
    sigset_t sigmask;  // Spawn with this signal mask
    bool sigmask_set;  // Whether `sigmask` should be used
    int thread_class;  // APTH_CLASS_DEFAULT/IO_BOUND/CPU_BOUND/REALTIME
};

#define APTH_ATTR_CAST(attr_ptr) ((struct apth_attr_st *)(attr_ptr))
#define APTH_ATTR_CONST_CAST(attr_ptr) ((const struct apth_attr_st *)(attr_ptr))

#define ATTR_FLAG_DETACHSTATE 0x0001
#define ATTR_FLAG_NOTINHERITSCHED 0x0002
#define ATTR_FLAG_SCOPEPROCESS 0x0004 // TODO: not supported in libapth
#define ATTR_FLAG_STACKADDR 0x0008
#define ATTR_FLAG_OLDATTR 0x0010
#define ATTR_FLAG_SCHED_SET 0x0020
#define ATTR_FLAG_POLICY_SET 0x0040 // TODO: not supported in libapth
#define ATTR_FLAG_DO_RSEQ 0x0080

// Returns 0 if ST is a valid stack size for a thread stack and EINVAL otherwise.
INLINE int check_stacksize_attr(size_t st)
{
    if (st >= APTH_STACK_SIZE_DEFAULT)
        return 0;

    return EINVAL;
}
APTH_INTERNAL int apth_attr_setsigmask_internal(apth_attr_t *attr, const sigset_t *sigmask);

#endif // __LIBAPTH_ATTR_APTH_ATTR_H
