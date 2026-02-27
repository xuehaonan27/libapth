#include "common.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"

struct apth_global_sigaction APTH_GLOBAL_SIGACTIONS;

APTH_INTERNAL int apth_signal_system_init(void)
{
    lll_init(&APTH_GLOBAL_SIGACTIONS.lock);

    for (int sig = 1; sig < APTH_NSIG; sig++)
    {
        APTH_GLOBAL_SIGACTIONS.actions[sig].sa_handler = SIG_DFL;
        sigemptyset(&APTH_GLOBAL_SIGACTIONS.actions[sig].sa_mask);
        APTH_GLOBAL_SIGACTIONS.actions[sig].sa_flags = 0;
    }

    return 0;
}

APTH_INTERNAL int apth_signal_system_drop(void)
{
    // Clear
    for (int sig = 1; sig < APTH_NSIG; sig++)
    {
        APTH_GLOBAL_SIGACTIONS.actions[sig].sa_handler = SIG_DFL;
        sigemptyset(&APTH_GLOBAL_SIGACTIONS.actions[sig].sa_mask);
        APTH_GLOBAL_SIGACTIONS.actions[sig].sa_flags = 0;
    }

    return 0;
}

// Default behaviour of `sig`.
static void __apth_sig_default_action(apth_t th, int sig)
{
    (void)th;
    switch (sig)
    {
    // Signals that will terminate the process
    case SIGTERM:
    case SIGINT:
    case SIGHUP:
    case SIGPIPE:
    case SIGALRM:
    case SIGUSR1:
    case SIGUSR2:
    case SIGPROF:
    case SIGVTALRM:
        // Default behaviour: terminate whole process.
        // Restore kernel default processing and redeliver the
        // signal.
        {
            struct sigaction dfl = {.sa_handler = SIG_DFL};
            apth_syscall_raw(sigaction)(sig, &dfl, NULL);
            apth_syscall_raw(raise)(sig);
        }
        break;

    // Signals that produces core dump
    case SIGQUIT:
    case SIGABRT:
    case SIGSEGV:
    case SIGBUS:
    case SIGFPE:
    case SIGILL:
    case SIGSYS:
    case SIGTRAP:
    case SIGXCPU:
    case SIGXFSZ:
    {
        struct sigaction dfl = {.sa_handler = SIG_DFL};
        apth_syscall_raw(sigaction)(sig, &dfl, NULL);
        apth_syscall_raw(raise)(sig);
    }
    break;

    // Ignored signals
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
        // Default behavious is to ignore
        break;

    // Signals indicating termination
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        // Stop the process
        apth_syscall_raw(raise)(sig);
        break;

    // Signal telling us to continue
    case SIGCONT:
        // If the process is stopped then we continue.
        // Else we ignore it.
        break;

    default:
        break;
    }
}

// Execute handler `sa` for signal `sig` in `th`'s context.
// There are 2 ways to do this.
//
// Plan A: we assume that all signal handlers are executed ONLY before the
// scheduler pass control flow to an apth (`th`), after setting cur_apth to
// `th`. Since program should never make assumption about when an asynchronous
// signal should be delivered, handling signals before scheduling the apth is
// acceptable. The signal handler is actually executed on scheduler's stack, but
// logically the handler belongs to `th`. The drawback is that the signal
// stack set by user is actually not used, so `sigaltstack` becomes a fake
// implementation.
//
// Plan B: mimicking kernel's behaviour. We first modify `th`'s ucontext,
// pointing it to a trampoline (e.g. we could call it `signal_trampoline`),
// and then the `signal_trampoline` will invoke handler(sig). And the
// trampoline should have its own stack. This is much more like what the OS
// kernel would do, but is more complicated and a little bad for space locality.
static void __apth_inject_signal_handler(apth_t th, int sig, struct sigaction *sa)
{
    // NOTE: here we first implement Plan A
    assert_msg(!APTH_IS_FAKE_SCHED(cur_apth()), "we should theoratically be an apth now");
    // Uh, although physically not yet. That does not matter though, because
    // we are meant to use scheduler's stack here. What matters is that
    // logically we should be an apth here (cur_apth set to th), because we
    // do not want any operation in the signal handler destroy scheduler's
    // status.

    // Block signals in sa->sa_mask temporarily
    sigset_t old_mask = th->sigmask;
    for (int s = 1; s < APTH_NSIG; s++)
        if (sigismember(&sa->sa_mask, s))
            sigaddset(&th->sigmask, s);

    // If SA_NODEFER is not set, block current signal as well
    if (!(sa->sa_flags & SA_NODEFER))
        sigaddset(&th->sigmask, sig);

    // Mark we are now in a signal handler
    th->in_sighandler = true;

    // Execute the handler
    if (sa->sa_flags & SA_SIGINFO)
    {
        // NOTE: in pthread implementation, the siginfo_t * is provided by
        // kernel, with necessary information. If we want to mimic the kernel
        // behaviour as much as possible, then we should provide the handler
        // with enough information. That's a big deal.
        // TODO: implement above.
        //
        // For now, construct a minimal siginfo_t *
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        info.si_signo = sig;
        info.si_code = SI_USER;

        sa->sa_sigaction(sig, &info, NULL);
    }
    else
        sa->sa_handler(sig);

    // Restore status
    th->in_sighandler = false;
    th->sigmask = old_mask;

    // If SA_RESETHAND, then modify to default handler
    if (sa->sa_flags & SA_RESETHAND)
    {
        struct sigaction dfl = {.sa_handler = SIG_DFL};
        lll_lock(&APTH_GLOBAL_SIGACTIONS.lock, "__apth_inject_signal_handler");
        APTH_GLOBAL_SIGACTIONS.actions[sig] = dfl;
        lll_unlock(&APTH_GLOBAL_SIGACTIONS.lock, "__apth_inject_signal_handler");
    }

    // TODO: implement Plan B and give this function a conditional compilation option
}

APTH_INTERNAL void apth_deliver_pending_signals(apth_t th)
{
    lll_lock(&th->siglock, "deliver_pending");

    // Fetch signals that in `pending & ~sigmask`
    for (int sig = 1; sig < APTH_NSIG; sig++)
    {
        if (!sigismember(&th->sigpending, sig))
            continue;
        if (sigismember(&th->sigmask, sig))
            continue; // blocked by sigmas

        // Remove this signal from pending set
        sigdelset(&th->sigpending, sig);
        th->sigpendcnt--;

        // Search handler
        lll_lock(&APTH_GLOBAL_SIGACTIONS.lock, "apth_deliver_pending_signals");
        struct sigaction sa = APTH_GLOBAL_SIGACTIONS.actions[sig];
        lll_unlock(&APTH_GLOBAL_SIGACTIONS.lock, "apth_deliver_pending_signals");

        if (sa.sa_handler == SIG_IGN)
            continue; // this signal is ignored, skip
        if (sa.sa_handler == SIG_DFL)
        {
            // default behaviour
            __apth_sig_default_action(th, sig);
            continue;
        }

        // This is signal is with user handler
        // NOTE: the handler should be executed in `apth`'s context
        // We must modify `th`'s ucontext, let it jump to a trampoline
        // and the trampoline code will call user's handler, and then
        // it will jump back to original PC
        __apth_inject_signal_handler(th, sig, &sa);
    }

    lll_unlock(&th->siglock, "deliver_pending");
}

// Process level pending signals, storing signals when all apths are blocking
// certain signals
// static sigset_t APTH_PROCESS_SIGPENDING;
static _Atomic(int) APTH_PROCESS_SIGPENDING[APTH_NSIG];
// static lll_t APTH_PROCESS_SIGPENDING_LOCK;

// Go throught ALL apths in ALL schedulers, and find its first apths that
// do not block the signal. If found, then set pending.
// If not, then set process-level pending.
//
// Note that we should not call function like `cur_apth` `cur_sched` `cur_worker`,
// since we are not in any pthread's context when executing this function.
// Instead, we are in a kernel level signal stack.
static void apth_route_process_signal(int sig)
{
    // NOTE: quite a situation. We do not in any pthread's context, so TLS not
    // initialized and invalid. And will thread local initialized if we enabled
    // conditional compilation flag APTH_CUR_USING_KEYWORD? I don't think so.
    // We are here, at nowhere, a signal stack.
    // BUT LLL LOCKING AND UNLOCKING NEED `cur_apth` `cur_sched` and `cur_worker`.
    // How to solve this problem ? We need to synchronize accessing to each
    // scheduler's apth queue.
    // Moreover, acquiring lll here is deadlock-prone.

    // Signal-safe context. We could only use async-signal-safe operations.

    // NOTE: The easiest way is setting signals to process level pending set, and
    // let schedulers checking them and pick signals they like at scheduling
    // point.

    // NOTE: we must use atomic operation or sig_atomic_t to set pending bit safely.
    if (sig >= 1 && sig < APTH_NSIG)
        atomic_store_release(&APTH_PROCESS_SIGPENDING[sig], 1);

    // NOTE: another plan is to write into a eventfd or pipe, waking up a scheduler
    // that's currently blocked in `select`. `write` itself is signal-safe.
    // But this method is way too complicated, and may cause more overhead.
}

struct __apth_pass_signal
{
    int sig;
};

static bool __apth_could_receive_this_signal(apth_t th, void *varg)
{
    struct __apth_pass_signal *arg = (struct __apth_pass_signal *)varg;
    return (!sigismember(&th->sigmask, arg->sig));
}

APTH_INTERNAL void apth_check_process_signals(apth_sched_t sched)
{
    for (int sig = 1; sig < APTH_NSIG; sig++)
    {
        // NOTE: after CAS fetched and removed the signal and before placing
        // it back, another scheduler is checking it, the latter would miss
        // the signal, in extreme situation this might cause high signal delivery
        // latency. But that's not a big deal though, an acceptable tradeoff.
        int expected = 1;
        if (!atomic_compare_exchange_acquire(&APTH_PROCESS_SIGPENDING[sig], &expected, 0))
            // We fail getting this signal, well continue check next one
            continue;

        // Try to give this signal to a certain apth on this scheduler that
        // do not block it. Go throught ready queue, waked queue, new queue
        // and running queue (cur).
        apth_t target = APTH_NULL;

        // First check apth that's going to be scheduled
        if (sched->cur != NULL && !APTH_IS_FAKE_SCHED(sched->cur))
        {
            if (!sigismember(&sched->cur->sigmask, sig))
                target = sched->cur;
        }

        // If not found, then try other queues.
        struct __apth_pass_signal aux;
        aux.sig = sig;
        if (target == APTH_NULL)
            target = find_first_in_thqueue(sched->ready_queue, __apth_could_receive_this_signal, &aux);
        if (target == APTH_NULL)
            target = find_first_in_thqueue(sched->waked_queue, __apth_could_receive_this_signal, &aux);
        if (target == APTH_NULL)
            target = find_first_in_thqueue(sched->new_queue, __apth_could_receive_this_signal, &aux);

        if (target == APTH_NULL)
        {
            // This scheduler do not have an apth fit for this signal
            // then put the signal back for other schedulers.
            atomic_store_release(&APTH_PROCESS_SIGPENDING[sig], 1);
            continue;
        }

        // Add to apth pending
        // sigdelset(&APTH_PROCESS_SIGPENDING, sig);
        lll_lock(&target->siglock, "route_sig");
        if (!sigismember(&target->sigpending, sig))
        {
            sigaddset(&target->sigpending, sig);
            target->sigpendcnt++;
        }
        lll_unlock(&target->siglock, "route_sig");
    }
}

// For signals that are generated internally in same process, by `apth_kill`,
// then the scheduler, `apth_kill` and hooked libc functions will take care of
// them.
// But for signals that are:
// 1. Cannot be blocked, or generated by hardware, like SIGKILL, SIGSTOP,
//    SIGILL, SIGBUS, etc (see comment of hooked libc sigaction)
// 2. Signals that's send by other processes by `kill` or `sigqueue`, the process
//    should select a proper thread and deliver the signal to it.
// If the process (instead of an apth) receives a signal, we should set to to
// a certain apth's pending set, via a global kernel level trampoline handler.
//
// So this is the trampoline. This function should be registered to
// APTH_GLOBAL_SIGACTIONS.
static void apth_kernel_signal_catcher(int sig, siginfo_t *info, void *uctx)
{
    // Route the signal to certain apth.
    // Policy: find the first apth that do not block this signal, and set its
    // pending bit.
    // If all apths block this signal, then set it to process level pending.
    (void)info;
    (void)uctx;
    apth_route_process_signal(sig);
}

APTH_INTERNAL int apth_install_kernel_signal_catchers(void)
{
    struct sigaction sa;
    sigset_t all_signals;

    // Block all signals when we are executing signal catcher
    sigfillset(&all_signals);
    sa.sa_sigaction = apth_kernel_signal_catcher;
    sa.sa_mask = all_signals;
    sa.sa_flags = SA_SIGINFO; // Using sa_sigaction field

    // int max_sig = sysconf(_SC_NSIG);
    // if (max_sig == -1)
    // {
    //     max_sig = 64;
    // }

    int ret = 0;

    for (int sig = 1; sig < APTH_NSIG; sig++)
    {
        if (sig == SIGKILL || sig == SIGSTOP || sig == 32 || sig == 33)
            continue;
        if (sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL || sig == SIGTRAP || sig == SIGSYS)
            continue;
        if (apth_syscall_raw(sigaction)(sig, &sa, NULL) == -1)
        {
            apth_debug("signal %d cound not be set handler with `sigaction`", sig);
            ret = -1;
            break;
        }
    }

    return ret;
}
