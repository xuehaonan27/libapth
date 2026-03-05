#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_getstack(const apth_attr_t *attr, void **stackaddr, size_t *stacksize)
{
    const struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    // Store the result
#if APTH_STACKGROWTH < 0 // _STACK_GROWS_DOWN
    *stackaddr = (char *)iattr->stackaddr - iattr->stacksize;
#else
    *stackaddr = (char *)iattr->stackaddr;
#endif
    *stacksize = iattr->stacksize;

    return 0;
}