#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_setscope(apth_attr_t *attr, int scope)
{
    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    switch (scope)
    {
    case APTH_SCOPE_SYSTEM:
        iattr->flags &= ~ATTR_FLAG_SCOPEPROCESS;
        break;
    case APTH_SCOPE_PROCESS:
        iattr->flags |= ATTR_FLAG_SCOPEPROCESS;
        break;
    default:
        return EINVAL;
    }

    return 0;
}