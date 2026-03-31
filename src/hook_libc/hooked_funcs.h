#ifndef __LIBAPTH_HOOK_LIBC_HOOKED_FUNCS_H
#define __LIBAPTH_HOOK_LIBC_HOOKED_FUNCS_H

#include "hook_lowlevel_io.h"
#ifndef APTH_LIST_OF_HOOK_LOWLEVEL_IO
#error "Must define APTH_LIST_OF_HOOK_LOWLEVEL_IO"
#endif

// Always include all hook headers — they provide type declarations and
// extern references needed by internal code (apth_worker.c, apth_create.c).
// The JVM build only varies which .c files are compiled, not which headers
// are visible.
#include "hook_process.h"
#ifndef APTH_LIST_OF_HOOK_PROCESS
#error "Must define APTH_LIST_OF_HOOK_PROCESS"
#endif

#include "hook_pthread.h"
#ifndef APTH_LIST_OF_HOOK_PTHREAD
#error "Must define APTH_LIST_OF_HOOK_PTHREAD"
#endif

#include "hook_signal.h"
#ifndef APTH_LIST_OF_HOOK_SIGNAL
#error "Must define APTH_LIST_OF_HOOK_SIGNAL"
#endif

#include "hook_socket.h"
#ifndef APTH_LIST_OF_HOOK_SOCKET
#error "Must define APTH_LIST_OF_HOOK_SOCKET"
#endif

#include "hook_time.h"
#ifndef APTH_LIST_OF_HOOK_TIME
#error "Must define APTH_LIST_OF_HOOK_TIME"
#endif

#ifdef APTH_JVM_BUILD
// JVM build: I/O + socket + time hooks only.
// Signal, process, and pthread hooks are excluded — HotSpot manages
// its own signals and thread creation via apth_create directly.
#define APTH_LIST_OF_HOOK_LIBC_FUNCTIONS \
    APTH_LIST_OF_HOOK_LOWLEVEL_IO        \
    APTH_LIST_OF_HOOK_SOCKET             \
    APTH_LIST_OF_HOOK_TIME
#else
#define APTH_LIST_OF_HOOK_LIBC_FUNCTIONS \
    APTH_LIST_OF_HOOK_LOWLEVEL_IO        \
    APTH_LIST_OF_HOOK_PROCESS            \
    APTH_LIST_OF_HOOK_PTHREAD            \
    APTH_LIST_OF_HOOK_SIGNAL             \
    APTH_LIST_OF_HOOK_SOCKET             \
    APTH_LIST_OF_HOOK_TIME
#endif

// ============================== Initialize and Drop ==============================
APTH_INTERNAL int apth_func_system_init(void);
APTH_INTERNAL int apth_func_system_drop(void);

#endif // __LIBAPTH_HOOK_LIBC_HOOKED_FUNCS_H
