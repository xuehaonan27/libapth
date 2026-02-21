#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_setschedparam(apth_attr_t *attr, const struct sched_param *param)
{
    apth_attr_t iattr = *attr;

    int ret = check_sched_priority_attr(param->sched_priority,
                                        iattr->schedpolicy);
    if (ret)
        return ret;

    /* Copy the new values.  */
    memcpy(&iattr->schedparam, param, sizeof(struct sched_param));

    /* Remember we set the value.  */
    iattr->flags |= ATTR_FLAG_SCHED_SET;

    return 0;
}