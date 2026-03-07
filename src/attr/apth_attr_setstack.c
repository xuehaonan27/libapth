#include "apth_attr.h"

int apth_attr_setstack(apth_attr_t *attr, void *stackaddr, size_t stacksize)
{
    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    // Catch invalid sizes
    int ret = check_stacksize_attr(stacksize);
    if (ret)
        return ret;

    iattr->stacksize = stacksize;
#if APTH_STACKGROWTH < 0 // _STACK_GROWS_DOWN
    iattr->stackaddr = (char *)stackaddr + stacksize;
#else  // _STACK_GROWS_DOWN
    iattr->stackaddr = (char *)stackaddr;
#endif // _STACK_GROWS_DOWN
    iattr->flags |= ATTR_FLAG_STACKADDR;

    return 0;
}