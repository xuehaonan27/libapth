#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"
#include <string.h>

int apth_setname_np(apth_t th, const char *name)
{
    if (name == NULL)
    {
        return apth_error(EINVAL, EINVAL);
    }

    size_t name_len = strlen(name);
    name_len = name_len > APTH_TCB_NAMELEN ? APTH_TCB_NAMELEN : name_len;
    memcpy(th->name, name, name_len);
    th->name[name_len + 1] = '\0';
    return 0;
}