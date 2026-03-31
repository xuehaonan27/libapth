#include "hooked_funcs.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include "utils/archplattoold.h"

// For JVM build, we also need to include the function lists from excluded
// hook headers (signal, process, pthread) because internal code uses their
// raw function pointers (e.g., pthread_attr_init, pthread_create).
#ifdef APTH_JVM_BUILD
#include "hook_process.h"
#include "hook_pthread.h"
#include "hook_signal.h"
#endif

APTH_INTERNAL int apth_func_system_init(void)
{
    apth_debug("enter");
    int fail_count = 0;
#define X(name)                                                             \
    if (apth_func_init(name)() != 0)                                        \
    {                                                                       \
        apth_debug("optional symbol not found: `%s`", stringify(name));     \
        fail_count++;                                                       \
    }
    // Initialize hooked functions (I/O + socket + time in JVM build)
    APTH_LIST_OF_HOOK_LIBC_FUNCTIONS
#ifdef APTH_JVM_BUILD
    // Also initialize excluded hooks' raw function pointers — internal
    // code (apth_worker.c, apth_create.c) calls pthread_* directly.
    APTH_LIST_OF_HOOK_PROCESS
    APTH_LIST_OF_HOOK_PTHREAD
    APTH_LIST_OF_HOOK_SIGNAL
#endif
#undef X
    apth_debug("leave (%d optional symbols missing)", fail_count);
    return 0;
}

APTH_INTERNAL int apth_func_system_drop(void)
{
    apth_debug("enter");
    apth_debug("leave");
    return 0;
}
