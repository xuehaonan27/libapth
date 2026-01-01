#include "internal_types.h"

struct apth_keytab_st {
    uintptr_t seq;
    void (*destructor)(void *);
};

