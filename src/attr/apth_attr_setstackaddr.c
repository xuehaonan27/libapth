#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_setstackaddr(apth_attr_t *attr, void *stackaddr)
{
    apth_attr_t iattr = *attr;

    iattr->stackaddr = stackaddr;
    iattr->flags |= ATTR_FLAG_STACKADDR;

    return 0;
}