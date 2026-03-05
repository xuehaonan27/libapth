#include "apth.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"

int apth_attr_setaffinity_np(apth_attr_t *attr, size_t cpusetsize, const cpu_set_t *cpuset)
{
    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    if (cpuset == NULL || cpusetsize == 0)
    {
        if (iattr->cpuset != NULL)
        {
            free(iattr->cpuset);
            iattr->cpuset = NULL;
            iattr->cpusetsize = 0;
        }
    }
    else
    {
        if (iattr->cpusetsize != cpusetsize)
        {
            void *newp = realloc(iattr->cpuset, cpusetsize);
            if (newp == NULL)
                return apth_error(ENOMEM, ENOMEM);

            iattr->cpuset = newp;
            iattr->cpusetsize = cpusetsize;
        }

        memcpy(iattr->cpuset, cpuset, cpusetsize);
    }

    return 0;
}