#ifndef __LIBAPTH_INTERNAL_APTH_DATA_H
#define __LIBAPTH_INTERNAL_APTH_DATA_H

#include "apth.h"
#include "utils/archplattoold.h"
#include <stdint.h>

struct apth_key_data
{
    uintptr_t seq; // Sequence number.
    void *data;    // Data pointer
};

struct apth_keytab_st
{
    // Sequence numbers. Even numbers indicated vacant entries,
    // Note that zero is even.
    _Atomic(uintptr_t) seq;
    // Destructor for the data.
    void (*destructor)(void *);
};

// Check whether an entry is unused.
#define APTH_KEY_UNUSED(p) (((p) & 1) == 0)

// Check whether a key is usable. We cannot reuse an allocated key if the
// sequence counter would overflow after the next destory call. This would
// mean that we potentially free memory for a key with the same sequence. This
// is very unlikely to happen, A program would have to create and destroy
// a key 2 ^ 31 (32-bits) or 2 ^ 63 (64-bits) times. If it should happen we
// simply don't use this specific key anymore.
#define APTH_KEY_USABLE(p) (((uintptr_t)(p)) < ((uintptr_t)((p) + 2)))

APTH_INTERNAL void apth_key_destroydata(apth_t th);

#endif // __LIBAPTH_INTERNAL_APTH_DATA_H
