#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"

int apth_attr_init(apth_attr_t *attr)
{
    struct apth_attr_st *iattr;

    // Many elements are initialized to zero so let us do it all at once.
    memset(attr, '\0', sizeof(struct apth_attr_st));

    iattr = (struct apth_attr_st *)attr;
    // Default guard size
    iattr->guardsize = page_size();

    return 0;
}
