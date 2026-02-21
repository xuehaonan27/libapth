#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_setstacksize(apth_attr_t *attr, size_t stacksize)
{
    apth_attr_t iattr = *attr;

    // Catch invalid sizes.
    int ret = check_stacksize_attr(stacksize);
    if (ret)
        return ret;

    iattr->stacksize = stacksize;

    return 0;
}