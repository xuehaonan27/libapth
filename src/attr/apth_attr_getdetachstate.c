#include "apth_attr.h"
#include "utils/apth_errno.h"

int apth_attr_getdetachstate(const apth_attr_t *attr, int *detachstate)
{
    const struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    *detachstate = (iattr->flags & ATTR_FLAG_DETACHSTATE
                        ? APTH_CREATE_DETACHED
                        : APTH_CREATE_JOINABLE);

    return 0;
}