#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

int apth_attr_getschedpolicy(const apth_attr_t *attr, int *policy)
{
    const apth_attr_t iattr = *attr;
    
    // Store the current values
    *policy = iattr->schedpolicy;

    return 0;
}