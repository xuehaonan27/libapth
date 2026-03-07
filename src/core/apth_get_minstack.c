#include "attr/apth_attr.h"

/* Get the minimum stack size for a thread.
 *
 * Unlike glibc's __pthread_get_minstack which includes:
 *   - page size
 *   - TLS static size
 *   - PTHREAD_STACK_MIN
 *
 * LIBAPTH allocates the TCB (Thread Control Block) separately from the stack,
 * so we don't need to account for TLS or TCB overhead in the stack size.
 *
 * The minimum stack size is simply APTH_MIN_STACK (16384 bytes), which
 * exncludes space for the guard page.
 */
size_t apth_get_minstack(const apth_attr_t *attr)
{
    (void)attr; // attr parameter is for compatibility with pthread API

    // In LIBAPTH, the minimum stack size is the default stack size
    // This is enforced in apth_tcb_alloc() and check_stacksize_attr()
    return APTH_MIN_STACK;
}
