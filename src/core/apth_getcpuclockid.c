#include "apth.h"
#include "common.h"
#include "hook_libc/hooked_funcs.h"
#include "internal/apth_tcb.h"
#include "internal/apth_sched.h"
#include "internal/apth_worker.h"
#include "utils/apth_errno.h"

/* Get the CPU-time clock ID for a thread.
 *
 * In pthread, this returns a clock ID that can be used with clock_gettime()
 * to measure CPU time consumed by a specific thread.
 *
 * For LIBAPTH, this is complex because:
 * 1. Multiple apths can run on the same pthread worker
 * 2. Apths are scheduled in userspace
 * 3. Tracking individual CPU time per apth would require significant overhead
 *
 * Current implementation: Return the clock ID of the underlying worker pthread
 * that the apth is currently scheduled on. This is not perfect but provides
 * some CPU time information.
 *
 * Alternative: Return ENOTSUP to indicate this feature is not supported.
 */
int apth_getcpuclockid(apth_t th, clockid_t *clock_id)
{
    if (!APTH_IS_VALID(th))
        return apth_error(ESRCH, ESRCH);
    if (clock_id == NULL)
        return apth_error(EINVAL, EINVAL);

    // Get the scheduler this apth belongs to
    apth_sched_t sched = SCHED_OF(th);
    if (sched == NULL)
        return apth_error(ESRCH, ESRCH);

    // Get the worker pthread
    apth_worker_t worker = sched->worker;
    if (worker == NULL)
        return apth_error(ESRCH, ESRCH);

    // Get the CPU clock ID for the underlying pthread worker
    // Note: This returns the CPU time for the entire worker, not just this apth
    int ret = apth_func_raw(pthread_getcpuclockid)(worker->tid, clock_id);
    if (ret != 0)
        return apth_error(ret, ret);

    return 0;
}
