#include "apth_data.h"
#include "apth.h"
#include "internal/types.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include <malloc.h>

// Maximum number of destructor iterations per POSIX.
// Destructors may set other keys, so we iterate until either all keys
// are NULL or we reach the iteration limit.
#define APTH_DESTRUCTOR_ITERATIONS 4

// Reference to the global key table (defined in core/apth_data.c)
extern struct apth_keytab_st APTH_KEYS[];

APTH_INTERNAL void apth_key_destroydata(apth_t th)
{
    if (th == NULL)
        return;
    if (!th->specific_used)
        return;

    // POSIX requires up to PTHREAD_DESTRUCTOR_ITERATIONS passes.
    // Each pass calls destructors for non-NULL keys, because a
    // destructor might set another key's value.
    for (int round = 0; round < APTH_DESTRUCTOR_ITERATIONS; round++)
    {
        bool any_found = false;

        for (size_t key = 0; key < APTH_KEYS_MAX; key++)
        {
            // Get the key's current sequence number
            uintptr_t gseq = atomic_load_acquire(&APTH_KEYS[key].seq);

            // Skip unused keys (even sequence = vacant)
            if (APTH_KEY_UNUSED(gseq))
                continue;

            // Get the destructor (may be NULL even for allocated keys)
            void (*destr)(void *) = APTH_KEYS[key].destructor;
            if (destr == NULL)
                continue;

            // Find the thread's data for this key
            struct apth_key_data *data;
            if (key < APTH_KEY_2NDLEVEL_SIZE)
            {
                data = &th->specific_1stblock[key];
            }
            else
            {
                unsigned int idx1st = key / APTH_KEY_2NDLEVEL_SIZE;
                struct apth_key_data *level2 = th->specific[idx1st];
                if (level2 == NULL)
                    continue;
                unsigned int idx2nd = key % APTH_KEY_2NDLEVEL_SIZE;
                data = &level2[idx2nd];
            }

            // Check if data is non-NULL and sequence matches
            void *val = data->data;
            if (val == NULL)
                continue;
            if (data->seq != gseq)
                continue;

            // Clear the data BEFORE calling destructor (POSIX semantics)
            data->data = NULL;
            any_found = true;

            // Call the destructor
            destr(val);
        }

        if (!any_found)
            break;
    }

    // Free dynamically allocated second-level arrays
    for (unsigned int i = 1; i < APTH_KEY_1STLEVEL_SIZE; i++)
    {
        if (th->specific[i] != NULL)
        {
            free(th->specific[i]);
            th->specific[i] = NULL;
        }
    }

    th->specific_used = false;
}
