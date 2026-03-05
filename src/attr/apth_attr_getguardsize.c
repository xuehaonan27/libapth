#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_getguardsize(const apth_attr_t *attr, size_t *guardsize)
{
    const struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    *guardsize = iattr->guardsize;

    return 0;
}