#include "apth.h"
#include "internal/types.h"
#include "utils/apth_errno.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"

// Cancel an apth. It sends a cancelation request to the apth. Whether and
// when the target apth reacts to the cancelation request depends on two
// attirbutes that are under the control of that thread: its cancelability
// state and type.
int apth_cancel(apth_t th)
{
    if (!APTH_IS_VALID(th))
        return apth_error(ESRCH, ESRCH);

    if (atomic_load_acquire(&th->state) == APTH_STATE_TERMINATED)
        return 0;

    // Now mark the thread as cancelled
    atomic_store_release(&th->cancelreq, true);

    return 0;
}
