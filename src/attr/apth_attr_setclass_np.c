#include "apth_attr.h"

int apth_attr_setclass_np(apth_attr_t *attr, int thread_class)
{
    if (attr == NULL)
        return EINVAL;
    if (thread_class != APTH_CLASS_DEFAULT &&
        thread_class != APTH_CLASS_IO_BOUND &&
        thread_class != APTH_CLASS_CPU_BOUND &&
        thread_class != APTH_CLASS_REALTIME &&
        thread_class != APTH_CLASS_DEDICATED &&
        thread_class != APTH_CLASS_DISTRIBUTED)
        return EINVAL;

    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);
    iattr->thread_class = thread_class;
    return 0;
}

int apth_attr_getclass_np(const apth_attr_t *attr, int *thread_class)
{
    if (attr == NULL || thread_class == NULL)
        return EINVAL;

    const struct apth_attr_st *iattr = APTH_ATTR_CONST_CAST(attr);
    *thread_class = iattr->thread_class;
    return 0;
}
