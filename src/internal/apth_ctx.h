#ifndef __LIBAPTH_INTERNAL_APTH_CTX_H
#define __LIBAPTH_INTERNAL_APTH_CTX_H

#include "utils/archplattoold.h"
#include <ucontext.h>
#include <stdbool.h>

// APTH Thread context
struct apth_cxt_st
{
    ucontext_t uc;
    int error;
    bool restored;
};
typedef struct apth_cxt_st *apth_cxt_t;

APTH_INTERNAL apth_cxt_t apth_ctx_alloc(void);
APTH_INTERNAL bool apth_ctx_save(apth_cxt_t ctx);
APTH_INTERNAL void apth_ctx_restore(apth_cxt_t ctx);
APTH_INTERNAL void apth_ctx_switch(apth_cxt_t old, apth_cxt_t new);
APTH_INTERNAL bool apth_ctx_set(apth_cxt_t ctx, void (*func)(void),
                                char *stack_mem_start, size_t stacksize);

#endif // __LIBAPTH_INTERNAL_APTH_CTX_H
