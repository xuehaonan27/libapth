#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"
#include <malloc.h>
#include <string.h>

int apth_attr_init(apth_attr_t *attr)
{
    apth_attr_t iattr = (apth_attr_t)malloc(sizeof(struct apth_attr_st));
    if (iattr == NULL)
    {
        return apth_error(ENOMEM, ENOMEM);
    }

    // Many elements are initialized to zero so let us do it all at once.
    memset(iattr, '\0', sizeof(struct apth_attr_st));

    // Default guard size
    iattr->guardsize = page_size();
    iattr->stackaddr = NULL;
    iattr->stacksize = APTH_STACK_SIZE_DEFAULT;

    *attr = iattr;
    return 0;
}
