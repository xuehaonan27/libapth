#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_errno.h"
#include <string.h>

int apth_attr_setname_np(apth_attr_t *attr, const char *name)
{
    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);
    if (iattr == NULL || name == NULL)
    {
        return apth_error(EINVAL, EINVAL);
    }

    size_t namelen = strlen(name);
    namelen = namelen > APTH_TCB_NAMELEN ? APTH_TCB_NAMELEN : namelen;
    memcpy(iattr->name, name, namelen);
    iattr->name[namelen + 1] = '\0';
    return 0;
}