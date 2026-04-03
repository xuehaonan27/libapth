#ifndef __LIBAPTH_INTERNAL_APTH_TCB_H
#define __LIBAPTH_INTERNAL_APTH_TCB_H

#include "internal/types/struct_apth_st.h"

// Yield reasons: why a thread yielded to the scheduler.
// The scheduler can use this for intelligent dispatch decisions.
#define APTH_YIELD_REASON_VOLUNTEER    ((uintptr_t)0x00)  // Explicit yield or I/O block
#define APTH_YIELD_REASON_WAIT         ((uintptr_t)0x02)  // Waiting for event (cond/sem/IO)
#define APTH_YIELD_REASON_TIMESLICE    ((uintptr_t)0x04)  // Preempted by timer
#define APTH_YIELD_REASON_EXIT         ((uintptr_t)0x08)  // Thread exit
// Phase 5: Extended yield reasons for JVM semantic-aware scheduling
#define APTH_YIELD_REASON_RDMA_WAIT    ((uintptr_t)0x10)  // Waiting for RDMA completion
#define APTH_YIELD_REASON_IO_WAIT      ((uintptr_t)0x20)  // Waiting for I/O completion
#define APTH_YIELD_REASON_SYNC_WAIT    ((uintptr_t)0x40)  // Waiting on mutex/cond/sem
#define APTH_YIELD_REASON_SAFEPOINT    ((uintptr_t)0x80)  // JVM safepoint wait
#define APTH_YIELD_REASON_GC_ASSIST    ((uintptr_t)0x100) // GC assistance work

APTH_INTERNAL apth_t apth_tcb_alloc(size_t stacksize, void *stackaddr, size_t guardsize);
APTH_INTERNAL void apth_tcb_free(apth_t t);
APTH_INTERNAL char *apth_tcb_get_usable_stack_start(apth_t t);

#endif // __LIBAPTH_INTERNAL_APTH_CTX_H
