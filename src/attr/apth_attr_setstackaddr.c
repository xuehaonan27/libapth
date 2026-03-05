#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_setstackaddr(apth_attr_t *attr, void *stackaddr)
{
    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    iattr->stackaddr = stackaddr;
    iattr->flags |= ATTR_FLAG_STACKADDR;

    return 0;
}