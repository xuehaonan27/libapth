#include "apth_attr.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_getscope(const apth_attr_t *attr, int *scope)
{
    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);
    /* Store the current values.  */
    *scope = (iattr->flags & ATTR_FLAG_SCOPEPROCESS
                  ? APTH_SCOPE_PROCESS
                  : APTH_SCOPE_SYSTEM);

    return 0;
}