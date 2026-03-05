#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_errno.h"
#include <string.h>

int apth_attr_getname_np(apth_attr_t *attr, char *buf, size_t len)
{
    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);
    if (iattr == NULL || buf == NULL)
    {
        return apth_error(EINVAL, EINVAL);
    }

    size_t namelen = len > APTH_TCB_NAMELEN ? APTH_TCB_NAMELEN : (len - 1);
    memcpy(buf, iattr->name, namelen);
    buf[namelen + 1] = '\0';
    return 0;
}