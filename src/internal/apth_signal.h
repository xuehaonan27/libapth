#ifndef __LIBAPTH_INTERNAL_APTH_SIGNAL_H
#define __LIBAPTH_INTERNAL_APTH_SIGNAL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <signal.h>

#include "internal/apth_sched.h"
#include "internal/apth_signal.h"
#include "utils/lll_new.h"
#include "utils/archplattoold.h"

#define APTH_NSIG 65

// Process level gloabl signal configure table
struct apth_global_sigaction
{
    struct sigaction actions[APTH_NSIG]; // Signal handler registered by user
    lll_internal_t lock;                 // for access protection
};
extern struct apth_global_sigaction APTH_GLOBAL_SIGACTIONS;

APTH_INTERNAL int apth_signal_system_init(void);
APTH_INTERNAL int apth_signal_system_drop(void);
APTH_INTERNAL void apth_deliver_pending_signals(apth_t th);
APTH_INTERNAL void apth_check_process_signals(apth_sched_t sched);
APTH_INTERNAL int apth_install_kernel_signal_catchers(void);

#endif // __LIBAPTH_INTERNAL_APTH_SIGNAL_H
