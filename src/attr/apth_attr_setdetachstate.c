#include "apth_attr.h"
#include "utils/apth_errno.h"

int apth_attr_setdetachstate(apth_attr_t *attr, int detachstate)
{
    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    if (detachstate != APTH_CREATE_DETACHED && detachstate != APTH_CREATE_JOINABLE)
        return apth_error(EINVAL, EINVAL);

    // Set the flag. It is nonzero if threads are created detached
    if (detachstate == APTH_CREATE_DETACHED)
        iattr->flags |= ATTR_FLAG_DETACHSTATE;
    else
        iattr->flags &= ~ATTR_FLAG_DETACHSTATE;

    return 0;
}