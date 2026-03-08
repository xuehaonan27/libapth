#include "attr/apth_attr.h"
#include "internal/types.h"
#include "utils/apth_errno.h"
#include <string.h>

/* Get the attributes of an existing thread.
 *
 * This function retrieves the actual attributes of a running thread,
 * including stack size, stack address, detach state, etc.
 *
 * Note: The attr parameter must be initialized with apth_attr_init()
 * before calling this function.
 */
int apth_getattr_np(apth_t th, apth_attr_t *attr)
{
    if (!APTH_IS_VALID(th))
        return apth_error(ESRCH, ESRCH);
    if (attr == NULL)
        return apth_error(EINVAL, EINVAL);

    struct apth_attr_st *iattr = APTH_ATTR_CAST(attr);

    // Copy stack information
    iattr->stacksize = th->stacksize;

    // Calculate stackaddr (top of stack for APTH_STACKGROWTH < 0)
#if APTH_STACKGROWTH < 0
    // Stack grows downward: stackaddr is the high address
    iattr->stackaddr = (void *)(th->stack_mem_start + th->stacksize);
#else
    // Stack grows upward: stackaddr is the low address
    iattr->stackaddr = (void *)th->stack_mem_start;
#endif

    // Set the STACKADDR flag if stack address is set
    if (th->stack_mem_start != NULL)
        iattr->flags |= ATTR_FLAG_STACKADDR;

    // Copy detach state
    if (IS_DETACHED(th))
        iattr->flags |= ATTR_FLAG_DETACHSTATE;
    else
        iattr->flags &= ~ATTR_FLAG_DETACHSTATE;

    // Copy thread name
    strncpy(iattr->name, th->name, APTH_TCB_NAMELEN);
    iattr->name[APTH_TCB_NAMELEN] = '\0';

    // Copy signal mask if set
    iattr->sigmask = th->sigmask;
    iattr->sigmask_set = true;

    // Guard size - LIBAPTH uses a simple guard (one uint32_t)
    // For compatibility, we report a minimal guard size
    iattr->guardsize = sizeof(uint32_t);

    // Scheduler parameters (not really used in LIBAPTH, but copy for compatibility)
    // These are placeholders as LIBAPTH doesn't use pthread scheduling
    iattr->schedpolicy = 0;
    iattr->schedparam.sched_priority = th->prio;

    return 0;
}
