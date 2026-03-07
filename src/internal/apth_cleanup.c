#include "apth_cleanup.h"
#include "apth.h"
#include "internal/apth_tcb.h"
#include "utils/apth_errno.h"
#include "utils/debug.h"
#include <stdbool.h>
#include <malloc.h>

static void apth_cleanup_popall(apth_t t, bool execute)
{
    apth_cleanup_t cleanup;

    while ((cleanup = t->cleanups) != NULL)
    {
        t->cleanups = cleanup->next;
        if (execute)
            cleanup->func(cleanup->arg);
        free(cleanup);
    }
    return;
}

// Cleanup a particular thread
APTH_INTERNAL void apth_thread_cleanup(apth_t th)
{
    // Run the cleaup handlers
    if (th->cleanups != NULL)
        apth_cleanup_popall(th, true);

    // Run the specific data destructors
    apth_key_destroydata(th);

    // Release still acquired synchronizing primitives
    // TODO("release sync primitives");
    return;
}
