#include "internal_funcs.h"
#include "internal_types.h"
#include "apth_errno.h"

#include <string.h>

int apth_setname_np(apth_t th, const char *name)
{
    size_t name_len = strlen(name);
    if (name_len >= APTH_TCB_NAMELEN)
        return apth_error(ERANGE, ERANGE);

    memcpy(th->name, name, name_len);

    return 0;
}