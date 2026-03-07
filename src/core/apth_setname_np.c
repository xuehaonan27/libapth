#include "common.h" // For APTH_TCB_NAMELEN
#include "apth.h"
#include "internal/apth_tcb.h"
#include "utils/apth_errno.h"
#include <string.h>

int apth_setname_np(apth_t th, const char *name)
{
    if (!APTH_IS_VALID(th))
        return apth_error(ESRCH, ESRCH);
    if (name == NULL)
        return apth_error(EINVAL, EINVAL);

    size_t name_len = strlen(name);
    if (name_len > APTH_TCB_NAMELEN)
        return apth_error(ERANGE, ERANGE);

    memcpy(th->name, name, name_len);
    th->name[name_len + 1] = '\0';
    return 0;
}