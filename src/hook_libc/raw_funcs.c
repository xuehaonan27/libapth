/**
 * raw_funcs.c — Raw libc function pointer resolver for LIBAPTH core build.
 *
 * This file provides all apth_func_raw_* function pointers and their
 * initialization, WITHOUT the libc-interposing hook wrappers.
 *
 * In the core build (APTH_CORE_BUILD), this replaces both the hook
 * wrapper files AND apth_hook_init_drop.c. No libc symbols (sigaction,
 * read, write, etc.) are exported, so the core library is safe to link
 * directly into applications like the JVM.
 *
 * Uses dlsym(RTLD_DEFAULT) instead of dlsym(RTLD_NEXT) because
 * RTLD_NEXT only works for interposition scenarios.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "apth.h"
#include "common.h"
#include "utils/debug.h"

/* Override the dlsym handle for core build */
#ifdef APTH_CORE_BUILD
#define APTH_DLSYM_HANDLE RTLD_DEFAULT
#else
#define APTH_DLSYM_HANDLE RTLD_NEXT
#endif

/*
 * For each hooked function, we need:
 *   1. A function pointer typedef: name_pfn_t
 *   2. A global variable: apth_func_raw_name
 *   3. An init function: apth_func_init_name()
 *
 * We reuse the existing declaration headers to get the typedefs,
 * then define the variables and init functions here.
 */

/* Include ALL hook headers for declarations and function lists */
#include "hook_libc/hook_lowlevel_io.h"
#include "hook_libc/hook_process.h"
#include "hook_libc/hook_pthread.h"
#include "hook_libc/hook_signal.h"
#include "hook_libc/hook_socket.h"
#include "hook_libc/hook_time.h"
#include "hook_libc/hooked_funcs.h"

/*
 * Define raw function pointer + initializer for each function.
 * This duplicates what APTH_FETCH_LIBCFUNC does, but uses
 * APTH_DLSYM_HANDLE instead of hardcoded RTLD_NEXT.
 */
#define stringify(x) #x

#define X(name) \
    apth_func_pfn_t(name) apth_func_raw(name) = NULL; \
    APTH_INTERNAL int apth_func_init(name)(void) { \
        if (apth_func_raw(name) != NULL) return 0; /* already initialized */ \
        apth_func_pfn_t(name) func = (apth_func_pfn_t(name))dlsym(APTH_DLSYM_HANDLE, stringify(name)); \
        if (func == NULL) { \
            apth_debug("raw_funcs: failed to find " stringify(name)); \
            return -1; \
        } \
        apth_func_raw(name) = func; \
        return 0; \
    }

APTH_LIST_OF_HOOK_LIBC_FUNCTIONS

#undef X

/*
 * System-wide initialization: resolve all raw function pointers.
 */
APTH_INTERNAL int apth_func_system_init(void)
{
    apth_debug("raw_funcs: system init");
    int fail_count = 0;

    // Resolve all raw function pointers via dlsym.
    // Some functions (close_range, closefrom, preadv64v2, pwritev64v2,
    // copy_file_range) may not exist on older glibc/kernel.  Missing
    // optional functions are tolerated — they remain NULL and callers
    // must check before use.  Core functions (read, write, open, close,
    // pthread_create, etc.) are fatal if missing.
#define X(name) \
    if (apth_func_init(name)() != 0) { \
        apth_debug("raw_funcs: optional symbol not found: " stringify(name)); \
        fail_count++; \
    }

    APTH_LIST_OF_HOOK_LIBC_FUNCTIONS

#undef X

    apth_debug("raw_funcs: system init complete (%d optional symbols missing)", fail_count);
    return 0;
}

/*
 * System-wide teardown: clear all raw function pointers.
 */
APTH_INTERNAL int apth_func_system_drop(void)
{
#define X(name) apth_func_raw(name) = NULL;
    APTH_LIST_OF_HOOK_LIBC_FUNCTIONS
#undef X
    return 0;
}
