#ifndef __LIBAPTH_INTERNAL_APTH_TCB_H
#define __LIBAPTH_INTERNAL_APTH_TCB_H

#include "internal/types/struct_apth_st.h"

#define APTH_YIELD_REASON_VOLUNTEER ((uintptr_t)0)
#define APTH_YIELD_REASON_WAIT ((uintptr_t)0x2)
#define APTH_YIELD_REASON_TIMESLICE ((uintptr_t)0x4)
#define APTH_YIELD_REASON_EXIT ((uintptr_t)0x8)

APTH_INTERNAL apth_t apth_tcb_alloc(size_t stacksize, void *stackaddr, size_t guardsize);
APTH_INTERNAL void apth_tcb_free(apth_t t);
APTH_INTERNAL char *apth_tcb_get_usable_stack_start(apth_t t);

#endif // __LIBAPTH_INTERNAL_APTH_CTX_H
