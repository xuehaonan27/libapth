#include "apth_attr.h"
#include "utils/apth_errno.h"
#include <malloc.h>

int apth_attr_destroy(apth_attr_t *attr)
{
    if (attr == NULL)
        return apth_error(EINVAL, EINVAL);

    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    if (iattr->cpuset != NULL)
        free(iattr->cpuset);

    // No free() needed for attr itself - it's stack-allocated
    return 0;
}