#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_errno.h"

int apth_attr_destroy(apth_attr_t *attr)
{
    if (attr == NULL)
        return apth_error(EINVAL, EINVAL);

    apth_attr_t iattr = *attr;
    if (iattr == NULL)
        return apth_error(EINVAL, EINVAL);

    if (iattr->cpuset != NULL)
        free(iattr->cpuset);

    free(iattr);
    return 0;
}