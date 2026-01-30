#include "internal_types.h"
#include "internal_funcs.h"
#include "apth_errno.h"
#include "archplattoold.h"

#define APTH_KEYS_MAX 1024

static struct apth_keytab_st APTH_KEYS[APTH_KEYS_MAX];

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
