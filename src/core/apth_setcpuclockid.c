#include "apth.h"
#include "utils/apth_errno.h"
#include "internal/types.h"
#include <bits/types/clockid_t.h>

/* Set the CPU-time clock ID for a thread.
 *
 * NOTE: This function is not part of the standard pthread API.
 * In pthread, there is no pthread_setcpuclockid() because CPU clock IDs
 * are read-only and assigned by the operating system.
 *
 * For LIBAPTH, this function is provided for API completeness but returns
 * ENOTSUP (not supported) because:
 * 1. CPU clock IDs are system-assigned and cannot be changed
 * 2. In LIBAPTH's M:N threading model, multiple apths share the same
 *    pthread worker, making per-apth CPU clock assignment meaningless
 * 3. The CPU clock ID is tied to the underlying pthread worker, not the apth
 *
 * If you need to measure CPU time for an apth, use apth_getcpuclockid()
 * to get the clock ID, then use clock_gettime() to read it.
 */
int apth_setcpuclockid(apth_t th, clockid_t clock_id)
{
    (void)clock_id; // Unused parameter

    if (!APTH_IS_VALID(th))
        return apth_error(ESRCH, ESRCH);

    // Setting CPU clock ID is not supported
    // CPU clock IDs are read-only and assigned by the system
    return apth_error(ENOTSUP, ENOTSUP);
}
