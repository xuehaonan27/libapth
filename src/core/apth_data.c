#include "internal/apth_data.h"
#include "apth.h"
#include "internal/types.h"
#include "utils/archplattoold.h"
#include "utils/atomic_wrapper.h"
#include "utils/apth_errno.h"
#include <stdlib.h>

static struct apth_keytab_st APTH_KEYS[APTH_KEYS_MAX];

int apth_key_create(apth_key_t *key, void (*destr)(void *))
{
    // Find a slot in APTH_KEYS which is unused.
    for (size_t cnt = 0; cnt < APTH_KEYS_MAX; ++cnt)
    {
        uintptr_t seq = APTH_KEYS[cnt].seq;
        if (APTH_KEY_UNUSED(seq) && APTH_KEY_USABLE(seq) &&
            // Found an unused slot, try to allocate it
            !atomic_compare_and_exchange_bool_acq(&APTH_KEYS[cnt].seq, seq + 1, seq))
        {
            // Remember the destructor
            APTH_KEYS[cnt].destructor = destr;
            // Return the key to the caller
            *key = cnt;
            // The call succeeded
            return 0;
        }
    }

    return apth_error(EAGAIN, EAGAIN);
}

int apth_key_delete(apth_key_t key)
{
    if (apth_likely(key < APTH_KEYS_MAX))
    {
        unsigned int seq = APTH_KEYS[key].seq;
        if (apth_expect(!APTH_KEY_UNUSED(seq), 1)
            // Acquire key
            && !atomic_compare_and_exchange_bool_acq(&APTH_KEYS[key].seq, seq + 1, seq))
        {
            return 0;
        }
    }

    return apth_error(EINVAL, EINVAL);
}

void *apth_getspecific(apth_key_t key)
{
    struct apth_key_data *data;

    // Special case access to the first 2nd-level block. This is the usual case.
    if (apth_likely(key < APTH_KEY_2NDLEVEL_SIZE))
        data = &CUR_APTH->specific_1stblock[key];
    else
    {
        // Verify the key is sane.
        if (key >= APTH_KEYS_MAX)
            return NULL;

        unsigned int idx1st = key / APTH_KEY_2NDLEVEL_SIZE;
        unsigned int idx2nd = key % APTH_KEY_2NDLEVEL_SIZE;

        // If the sequence number doesn't match or the key cannot be defined
        // for this apth since the second level array is not allocated,
        // Return NULL.
        struct apth_key_data *level2 = CUR_APTH->specific[idx1st];
        if (level2 == NULL)
            return NULL; // Not allocated, therefore no data

        // There is data
        data = &level2[idx2nd];
    }

    void *result = data->data;
    if (result != NULL)
    {
        uintptr_t seq = data->seq;
        if (apth_unlikely(seq != APTH_KEYS[key].seq))
            result = data->data = NULL;
    }
    return result;
}

int apth_setspecific(apth_key_t key, const void *value)
{
    apth_t self;
    unsigned int idx1st;
    unsigned int idx2nd;
    struct apth_key_data *level2;
    unsigned int seq;

    self = CUR_APTH;

    // Special case access to the first 2nd-level block. This is the usual case
    if (apth_likely(key < APTH_KEY_2NDLEVEL_SIZE))
    {
        // Verify the key is sane.
        if (APTH_KEY_UNUSED((seq = APTH_KEYS[key].seq)))
            return EINVAL; // Not valid

        level2 = &self->specific_1stblock[key];

        // Remember that we stored at least one set of data
        if (value != NULL)
            self->specific_used = true;
    }
    else
    {
        if (key >= APTH_KEYS_MAX || APTH_KEY_UNUSED((seq = APTH_KEYS[key].seq)))
            return EINVAL; // Not valid

        idx1st = key / APTH_KEY_2NDLEVEL_SIZE;
        idx2nd = key % APTH_KEY_2NDLEVEL_SIZE;

        // This is the second level array. Allocate it if necessary.
        level2 = self->specific[idx1st];
        if (level2 == NULL)
        {
            if (value == NULL)
                return 0;

            level2 = (struct apth_key_data *)calloc(APTH_KEY_2NDLEVEL_SIZE, sizeof(*level2));
            if (level2 == NULL)
                return ENOMEM;

            self->specific[idx1st] = level2;
        }

        // Pointer to the right array element.
        level2 = &level2[idx2nd];

        // Remember that we stored at least one set of data.
        self->specific_used = true;
    }

    // Store the data and the sequence number so that we can recognize stale data
    level2->seq = seq;
    level2->data = (void *)value;

    return 0;
}