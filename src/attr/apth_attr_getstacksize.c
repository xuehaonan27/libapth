#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_getstacksize(const apth_attr_t *attr, size_t *stacksize)
{
    const apth_attr_t iattr = *attr;

    size_t size = iattr->stacksize;

    // If the user has not set a stack size we return what the system
    // will use as the default.
    if (size == 0)
    {
        size = APTH_STACK_SIZE_DEFAULT;
    }

    *stacksize = size;

    return 0;
}