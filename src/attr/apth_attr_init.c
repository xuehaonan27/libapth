#include "apth_attr.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"
#include <string.h>

int apth_attr_init(apth_attr_t *attr)
{
    if (attr == NULL)
        return apth_error(EINVAL, EINVAL);

    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    // Many elements are initialized to zero so let us do it all at once.
    memset(iattr, '\0', sizeof(struct apth_attr_st));

    // Default guard size
    iattr->guardsize = page_size();
    iattr->stackaddr = NULL;
    iattr->stacksize = APTH_STACK_SIZE_DEFAULT;

    return 0;
}
