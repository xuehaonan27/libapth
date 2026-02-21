#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_setschedpolicy(apth_attr_t *attr, int policy)
{
    apth_attr_t iattr = *attr;

    // Catch invalid values.
    int ret = check_sched_policy_attr(policy);
    if (ret)
        return ret;

    /* Store the new values.  */
    iattr->schedpolicy = policy;

    /* Remember we set the value.  */
    iattr->flags |= ATTR_FLAG_POLICY_SET;

    return 0;
}