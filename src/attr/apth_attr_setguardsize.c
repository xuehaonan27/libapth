#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_setguardsize(apth_attr_t *attr, size_t guardsize)
{
    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    // GNU NPTL says here:
    /* Note that we don't round the value here.  The standard requires
     that subsequent pthread_attr_getguardsize calls return the value
     set by the user.  */
    iattr->guardsize = guardsize;

    return 0;
}