#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"
#include <string.h>

int apth_attr_copy(apth_attr_t *target, const apth_attr_t *source)
{
    if (target == NULL || source == NULL)
        return apth_error(EINVAL, EINVAL);

    apth_attr_t itarget = *target;
    apth_attr_t isource = *source;

    apth_attr_init(&itarget);

    memcpy(itarget, isource, sizeof(struct apth_attr_st));

    int ret = 0;
    if (isource->cpusetsize > 0)
    {
        // Propagate affinity mask information.
        ret = apth_attr_setaffinity_np(&itarget, isource->cpusetsize, isource->cpuset);

        // Propagate the signal mask information.
        if (ret == 0 && isource->sigmask_set)
            ret = apth_attr_setsigmask_internal(&itarget, &isource->sigmask);
    }

    if (ret != 0)
    {
        // Deallocate because we have ownership.
        apth_attr_destroy(&itarget);
        return ret;
    }

    // Transter ownership.
    *target = itarget;
    return 0;
}