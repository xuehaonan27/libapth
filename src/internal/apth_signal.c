#include "common.h"
#include "internal_funcs.h"
#include "internal_types.h"

static void apth_util_sigdelete_sighandler(int sig MAYBE_UNUSED) { /* nop */ return; }

// Delete the signal from this kernel thread
APTH_INTERNAL int apth_util_sigdelete(int sig)
{
    sigset_t ss, oss;
    struct sigaction sa, osa;

    // Check status of signal
    sigpending(&ss);
    if (!sigismember(&ss, sig))
        return -1;

    // Block signal and remember old mask
    sigemptyset(&ss);
    sigaddset(&ss, sig);
    apth_syscall_raw(pthread_sigmask)(SIG_BLOCK, &ss, &oss);

    // Set signal action to our dummy handler
    sa.sa_handler = apth_util_sigdelete_sighandler;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(sig, &sa, &osa) != 0)
    {
        apth_syscall_raw(pthread_sigmask)(SIG_SETMASK, &oss, NULL);
        return -1;
    }

    // Now let signal be delivered
    sigfillset(&ss);
    sigdelset(&ss, sig);
    sigsuspend(&ss);

    // Restore signal mask and handler
    sigaction(sig, &osa, NULL);
    apth_syscall_raw(pthread_sigmask)(SIG_SETMASK, &oss, NULL);
    return 0;
}
