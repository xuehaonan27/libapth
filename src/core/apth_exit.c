#include "apth.h"
#include "internal/types.h"
#include "internal/apth_cancel.h"
#include "internal/apth_dedicated.h"
#include "internal/apth_sched.h"
#include "hook_libc/hooked_funcs.h"
#include "utils/apth_errno.h"
#include "utils/atomic_wrapper.h"
#include "utils/debug.h"
#include <pthread.h>

// NOTE: in pthread, `pthread_exit` is just a thin wrapper around __do_cancel
void apth_exit(void *retval)
{
    apth_t cur = CUR_APTH;

    // Dedicated threads: use dedicated exit path, then terminate the pthread
    if (cur->is_dedicated)
    {
        if (cur == get_MAIN_APTH())
            atomic_store_release(&MAIN_APTH_EXITED_BY_CALLING_APTH_EXIT, 1);
        apth_dedicated_do_exit(retval);
        apth_func_raw(pthread_exit)(NULL);
        PANIC("Should not reach here");
    }

    if (cur == get_MAIN_APTH())
        // Main apth exit by calling `apth_exit`
        atomic_store_release(&MAIN_APTH_EXITED_BY_CALLING_APTH_EXIT, 1);

    // TODO: unwind?
    apth_do_cancel(retval);

    PANIC("Should not reach here");
}
