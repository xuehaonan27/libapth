#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"
#include "utils/debug.h"

// NOTE: in pthread, `pthread_exit` is just a thin wrapper around __do_cancel
void apth_exit(void *retval)
{

    // TODO: unwind?
    apth_do_cancel(retval);
}
