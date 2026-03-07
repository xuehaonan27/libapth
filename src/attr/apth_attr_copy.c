#include "apth_attr.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"
#include <string.h>

int apth_attr_copy(apth_attr_t *target, const apth_attr_t *source)
{
    if (target == NULL || source == NULL)
        return apth_error(EINVAL, EINVAL);

    struct apth_attr_st *itarget = APTH_ATTR_CAST(target);
    const struct apth_attr_st *isource = APTH_ATTR_CONST_CAST(source);

    apth_attr_init(target);

    memcpy(itarget, isource, sizeof(struct apth_attr_st));

    int ret = 0;
    if (isource->cpusetsize > 0)
    {
        // Propagate affinity mask information.
        ret = apth_attr_setaffinity_np(target, isource->cpusetsize, isource->cpuset);

        // Propagate the signal mask information.
        if (ret == 0 && isource->sigmask_set)
            ret = apth_attr_setsigmask_internal(target, &isource->sigmask);
    }

    if (ret != 0)
    {
        // Deallocate because we have ownership.
        apth_attr_destroy(target);
        return ret;
    }

    return 0;
}