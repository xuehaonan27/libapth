#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_getstackaddr(const apth_attr_t *attr, void **stackaddr)
{
    const apth_attr_t iattr = *attr;

    /* Some code assumes this function to work even if no stack address
     has been set.  Let them figure it out for themselves what the
     value means.  Simply store the result.  */
    *stackaddr = iattr->stackaddr;

    return 0;
}