#include "internal_types.h"
#include "internal_funcs.h"

int apth_key_create(apth_key_t *key, void (*destr)(void *))
{
    // Find a slot in APTH_KEYS which is unused
    for (size_t cnt = 0; cnt < APTH_KEYS_MAX; ++cnt)
    {
        uintptr_t seq = APTH_KEYS[cnt].seq;
        if (APTH_KEY_UNUSED(seq) && APTH_KEY_USABLE(seq)
            // We found an unused slot.  Try to allocate it.
            && !atomic_compare_and_exchange_bool_acq(&APTH_KEYS[cnt].seq, seq + 1, seq))
        {
            APTH_KEYS[cnt].destructor = destr;
            *key = cnt;
            return 0;
        }
    }

    return apth_error(EAGAIN, EAGAIN);
}
