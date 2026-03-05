#include "common.h"
#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_getsigmask_np(const apth_attr_t *attr, sigset_t *sigmask)
{
    const struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    if (!iattr->sigmask_set)
    {
        sigemptyset(sigmask);
        return APTH_ATTR_NO_SIGMASK_NP;
    }
    else
    {
        *sigmask = iattr->sigmask;
        return 0;
    }
}