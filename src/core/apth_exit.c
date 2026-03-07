#include "apth.h"
#include "internal/apth_tcb.h"
#include "internal/apth_cancel.h"
#include "utils/apth_errno.h"
#include "utils/atomic_wrapper.h"
#include "utils/debug.h"

// NOTE: in pthread, `pthread_exit` is just a thin wrapper around __do_cancel
void apth_exit(void *retval)
{
    apth_t cur = CUR_APTH;
    if (cur == get_MAIN_APTH())
        // Main apth exit by calling `apth_exit`
        atomic_store_release(&MAIN_APTH_EXITED_BY_CALLING_APTH_EXIT, 1);

    // TODO: unwind?
    apth_do_cancel(retval);

    PANIC("Should not reach here");
}
