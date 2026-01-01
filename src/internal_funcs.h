#ifndef __LIBAPTH_INTERNAL_FUNCS_H
#define __LIBAPTH_INTERNAL_FUNCS_H

#include "internal_types.h"

apth_t apth_tcb_alloc(size_t, void *);
void apth_tch_free(apth_t);

bool apth_cleanup_push(void (*)(void *), void *);
bool apth_cleanup_pop(bool);
void apth_cleanup_popall(apth_t, bool);

#endif /* __LIBAPTH_INTERNAL_FUNCS_H */
