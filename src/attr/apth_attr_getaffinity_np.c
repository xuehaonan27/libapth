#include "apth.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"

int apth_attr_getaffinity_np(const apth_attr_t *attr, size_t cpusetsize, cpu_set_t *cpuset)
{
    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    if (iattr->cpuset != NULL)
    {
        // Check whether there are any bits set beyond the limits the user requested
        for (size_t cnt = cpusetsize; cnt < iattr->cpusetsize; ++cnt)
            if (((char *)iattr->cpuset)[cnt] != 0)
                return apth_error(EINVAL, EINVAL);

        /* Copy over the cpuset from the thread attribute object.  Limit the copy
       to the minimum of the source and destination sizes to prevent a buffer
       overrun.  If the destination is larger, fill the remaining space with
       zeroes.  */
        size_t copysize = (iattr->cpusetsize < cpusetsize) ? iattr->cpusetsize : cpusetsize;
        memcpy(cpuset, iattr->cpuset, copysize);

        if (cpusetsize > iattr->cpusetsize)
        {
            void *p = (void *)(((void *)cpuset) + copysize);
            memset(p, '\0', cpusetsize - iattr->cpusetsize);
        }
    }
    else
        // We have no information
        memset(cpuset, -1, cpusetsize);

    return 0;
}