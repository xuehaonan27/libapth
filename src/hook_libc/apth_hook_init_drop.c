#include "hooked_funcs.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include "utils/archplattoold.h"

APTH_INTERNAL int apth_func_system_init(void)
{
    apth_debug("enter");
/*
#define X(name)                                                             \
    if (apth_func_init(name)() != 0)                                        \
    {                                                                       \
        apth_debug("fail to initialize system call `%s`", stringify(name)); \
        return -1;                                                          \
    }
*/
#define X(name)                                                             \
    if (apth_func_init(name)() != 0)                                        \
    {                                                                       \
        apth_debug("fail to initialize system call `%s`", stringify(name)); \
    }
    APTH_LIST_OF_HOOK_LIBC_FUNCTIONS
#undef X
    apth_debug("leave");
    return 0;
}

APTH_INTERNAL int apth_func_system_drop(void)
{
    apth_debug("enter");
    apth_debug("leave");
    return 0;
}
