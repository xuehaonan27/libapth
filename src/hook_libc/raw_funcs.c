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

/*
 * dlsym handle selection:
 *
 * RTLD_NEXT: "find the next definition after this library."
 *   Required for INTERPOSED functions — the hook wrapper (e.g. our read())
 *   needs the REAL libc read() behind it.  If we used RTLD_DEFAULT, we'd
 *   find our own hook and recurse infinitely.
 *
 * RTLD_DEFAULT: "find the first definition in global search order."
 *   Required for NON-INTERPOSED functions in the JVM build.  pthread_*
 *   and signal_* are not hooked, so RTLD_NEXT from libapth_jvm.so might
 *   skip them on older glibc (2.27) where libpthread.so is separate.
 *
 * Core build: everything uses RTLD_DEFAULT (no hooks exist, no
 *   interposition; the .a is statically linked into libjvm.so).
 *
 * Full build: everything uses RTLD_NEXT (all functions are hooked).
 *
 * JVM build: interposed functions use RTLD_NEXT (via hook files'
 *   APTH_FETCH_LIBCFUNC which overrides our weak definitions);
 *   non-interposed functions use RTLD_DEFAULT (from this file).
 */

/* Include ALL hook headers for declarations and function lists */
#include "hook_libc/hook_lowlevel_io.h"
#include "hook_libc/hook_process.h"
#include "hook_libc/hook_pthread.h"
#include "hook_libc/hook_signal.h"
#include "hook_libc/hook_socket.h"
#include "hook_libc/hook_time.h"
#include "hook_libc/hooked_funcs.h"

#define stringify(x) #x

// When APTH_RAW_FUNCS_WEAK is defined (JVM build), raw function pointer
// variables and init functions are declared weak so that hook files
// (which define strong symbols via APTH_DEFINE_HOOK with RTLD_NEXT)
// can override them for interposed functions.
#ifdef APTH_RAW_FUNCS_WEAK
#define RAW_ATTR __attribute__((weak))
#else
#define RAW_ATTR
#endif

// Core build: no hooks at all, use RTLD_DEFAULT for everything.
// Full build: hooks override weak defs, use RTLD_NEXT here too.
// JVM build (APTH_RAW_FUNCS_WEAK): hooks override interposed functions
//   with RTLD_NEXT; non-interposed functions keep our weak defs with
//   RTLD_DEFAULT.  So we use RTLD_DEFAULT here, which is correct for
//   the non-overridden (non-interposed) functions.  For interposed
//   functions, the hook's strong RTLD_NEXT definition wins at link time.
#ifdef APTH_CORE_BUILD
#define RAW_DLSYM_HANDLE RTLD_DEFAULT
#elif defined(APTH_RAW_FUNCS_WEAK)
#define RAW_DLSYM_HANDLE RTLD_DEFAULT
#else
#define RAW_DLSYM_HANDLE RTLD_NEXT
#endif

#define X(name) \
    RAW_ATTR apth_func_pfn_t(name) apth_func_raw(name) = NULL; \
    RAW_ATTR APTH_INTERNAL int apth_func_init(name)(void) { \
        if (apth_func_raw(name) != NULL) return 0; /* already initialized */ \
        apth_func_pfn_t(name) func = (apth_func_pfn_t(name))dlsym(RAW_DLSYM_HANDLE, stringify(name)); \
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
RAW_ATTR APTH_INTERNAL int apth_func_system_init(void)
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
RAW_ATTR APTH_INTERNAL int apth_func_system_drop(void)
{
#define X(name) apth_func_raw(name) = NULL;
    APTH_LIST_OF_HOOK_LIBC_FUNCTIONS
#undef X
    return 0;
}
