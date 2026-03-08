#ifndef __LIBAPTH_INTERNAL_TYPES_H
#define __LIBAPTH_INTERNAL_TYPES_H

#include "internal/types/struct_apth_event_st.h"
#include "internal/types/struct_apth_sched_st.h"
#include "internal/types/struct_apth_st.h"
#include "internal/types/struct_apth_thqueue_st.h"
#include "internal/types/struct_apth_worker_st.h"

INLINE void __type_size_check(void) {
    const size_of_struct_apth_st = sizeof(struct apth_st);
    const size_of_struct_event_st = sizeof(struct apth_event_st);
    const a = sizeof(sigset_t);
}

#endif // __LIBAPTH_INTERNAL_TYPES_H
