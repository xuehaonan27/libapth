#include "apth.h"
#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_sysutils.h"
#include "utils/apth_errno.h"

// NOTE: GNU NPTL implementation will clear internal signals
// (SIGCANCEL and SIGSETXID) here. But we might not need this.
static inline void clear_internal_signals(sigset_t *set)
{
    (void)set; // Make compiler happy
}

APTH_API int apth_attr_setsigmask_np(apth_attr_t *attr, const sigset_t *sigmask)
{
    int ret = apth_attr_setsigmask_internal(attr, sigmask);
    if (ret != 0)
        return ret;

    // Filter out internal signals
    apth_attr_t iattr = *attr;
    clear_internal_signals(&iattr->sigmask);
    return 0;
}

APTH_INTERNAL int apth_attr_setsigmask_internal(apth_attr_t *attr, const sigset_t *sigmask)
{
    apth_attr_t iattr = *attr;

    if (sigmask == NULL)
    {
        // Mark the signal mask as unset if it is present
        iattr->sigmask_set = false;
        return 0;
    }

    iattr->sigmask = *sigmask;
    iattr->sigmask_set = true;

    return 0;
}