#ifndef __LIBAPTH_HOOK_LIBC_HOOKED_FUNCS_H
#define __LIBAPTH_HOOK_LIBC_HOOKED_FUNCS_H

#include "hook_lowlevel_io.h"
#ifndef APTH_LIST_OF_HOOK_LOWLEVEL_IO
#error "Must define APTH_LIST_OF_HOOK_LOWLEVEL_IO"
#endif

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

#define APTH_LIST_OF_HOOK_LIBC_FUNCTIONS \
    APTH_LIST_OF_HOOK_LOWLEVEL_IO        \
    APTH_LIST_OF_HOOK_PROCESS            \
    APTH_LIST_OF_HOOK_PTHREAD            \
    APTH_LIST_OF_HOOK_SIGNAL             \
    APTH_LIST_OF_HOOK_SOCKET             \
    APTH_LIST_OF_HOOK_TIME

// ============================== Initialize and Drop ==============================
APTH_INTERNAL int apth_func_system_init(void);
APTH_INTERNAL int apth_func_system_drop(void);

#endif // __LIBAPTH_HOOK_LIBC_HOOKED_FUNCS_H
