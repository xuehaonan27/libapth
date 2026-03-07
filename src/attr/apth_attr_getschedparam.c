#include "apth_attr.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_getschedparam(const apth_attr_t *attr,
                            struct sched_param *param)
{
    const struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    /* Copy the current values.  */
    memcpy(param, &iattr->schedparam, sizeof(struct sched_param));

    return 0;
}