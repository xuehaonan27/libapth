#ifndef __LIBAPTH_HOOK_LIBC_HOOK_UTILS_H
#define __LIBAPTH_HOOK_LIBC_HOOK_UTILS_H

#include "utils/archplattoold.h"
#include "utils/debug.h"
#include "utils/apth_errno.h"
#include <dlfcn.h> // For dlsym and RTLD_NEXT

// Get identifier of LIBAPTH wrapped function, which is also exposed as a symbol of LIBAPTH.
// Whenever you want to use a LIBAPTH-hooked version of a function, wrap it in `apth_func`
#define apth_func(name) apth_func_##name
#define apth_func_raw(name) apth_func_raw_##name   // Get reference to real LIBC function
#define apth_func_init(name) apth_func_init_##name // Get reference to LIBC function initializer
#define apth_func_pfn_t(name) name##_pfn_t         // Get LIBC function pointer type
#define stringify(x) #x                            // Stringify the identifier `x`
#define apth_hook_debug(name) apth_debug("Hook " stringify(name) " succeed")

// Declare function prototype corresponding to that in LIBC
#define APTH_DECLARE_FETCH_LIBCFUNC(rettype, name, ...)    \
    typedef rettype (*apth_func_pfn_t(name))(__VA_ARGS__); \
    APTH_INTERNAL int apth_func_init(name)(void);          \
    extern apth_func_pfn_t(name) apth_func_raw(name);

// Declare LIBC function hook, should be used in a header file
#define APTH_DECLARE_HOOK(rettype, name, ...)               \
    APTH_DECLARE_FETCH_LIBCFUNC(rettype, name, __VA_ARGS__) \
    APTH_INTERNAL rettype apth_func(name)(__VA_ARGS__);

// Define function type and initializer fetching the original function in LIBC that's going to be hooked.
// Should be used in a `.c` file.
#define APTH_FETCH_LIBCFUNC(name)                                                              \
    MAYBE_UNUSED APTH_INTERNAL apth_func_pfn_t(name) apth_func_raw(name) = NULL;               \
    APTH_INTERNAL int apth_func_init(name)(void)                                               \
    {                                                                                          \
        assert_msg(apth_func_raw(name) == NULL, "sanity");                                     \
        apth_func_pfn_t(name) func = (apth_func_pfn_t(name))dlsym(RTLD_NEXT, stringify(name)); \
        if (func == NULL)                                                                      \
            return -1;                                                                         \
        apth_debug("found syscall " stringify(name) " = %p", func);                            \
        apth_func_raw(name) = func;                                                            \
        assert_msg(apth_func_raw(name) != NULL, "sanity");                                     \
        return 0;                                                                              \
    }

// Define a LIBC function hook, inserting logics before and after actually calling LIBC functions.
// Should be used in a `.c` file.
#define APTH_DEFINE_HOOK(rettype, name, typedargs, argnames) \
    APTH_FETCH_LIBCFUNC(name)                                \
    APTH_API rettype name typedargs                          \
    {                                                        \
        return apth_func(name) argnames;                     \
    }                                                        \
    APTH_INTERNAL rettype apth_func(name) typedargs

#endif // __LIBAPTH_HOOK_LIBC_HOOK_UTILS_H
