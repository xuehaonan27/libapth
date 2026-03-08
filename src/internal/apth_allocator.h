#ifndef __LIBAPTH_INTERNAL_APTH_ALLOCATOR_H
#define __LIBAPTH_INTERNAL_APTH_ALLOCATOR_H

#include "utils/list.h"
#include "utils/lll.h"
#include <stddef.h>

struct glbmem_st {
    lll_internal_t lock;
    void *start; // 
};

// Memory region private to a scheduler
// TODO: NUMA aware
struct pschemem_st
{
    void *start;
    size_t size;
    struct list st_holder_list;
};

struct struct_holder_st
{
    void *start;
    size_t size;
    size_t slot_size; // at least 1 word, 4B (32 bits) or 8B (64 bits)
    void *cur_top;
    uintptr_t *next_hole_addr;
    struct list_elem elem;
};

static void check_size(void) {
    const int a = sizeof(struct struct_holder_st);
}

#endif // __LIBAPTH_INTERNAL_APTH_ALLOCATOR_H
