#include "apth.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"

int apth_attr_getdetachstate(const apth_attr_t *attr, int *detachstate)
{
    const apth_attr_t iattr = *attr;

    *detachstate = (iattr->flags & ATTR_FLAG_DETACHSTATE
                        ? APTH_CREATE_DETACHED
                        : APTH_CREATE_JOINABLE);

    return 0;
}