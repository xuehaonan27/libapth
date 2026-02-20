#include "apth.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"

int apth_attr_setdetachstate(apth_attr_t *attr, int detachstate)
{
    apth_attr_t iattr = *attr;

    if (detachstate != APTH_CREATE_DETACHED && detachstate != APTH_CREATE_JOINABLE)
        return apth_error(EINVAL, EINVAL);

    // Set the flag. It is nonzero if threads are created detached
    if (detachstate == APTH_CREATE_DETACHED)
        iattr->flags |= ATTR_FLAG_DETACHSTATE;
    else
        iattr->flags &= ~ATTR_FLAG_DETACHSTATE;

    return 0;
}