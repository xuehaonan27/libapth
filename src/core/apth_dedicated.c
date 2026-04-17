#include "common.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_dedicated.h"
#include "internal/apth_cleanup.h"
#include "internal/apth_data.h"
#include "internal/apth_sched.h"
#include "hook_libc/hooked_funcs.h"
#include "utils/debug.h"
#include "internal/apth_stats.h"
#include "utils/atomic_wrapper.h"
#include "utils/lll.inline.h"
#include "utils/list.inline.h"
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>

// External reference to global thread count (defined in apth_sched.c)
extern _Atomic(unsigned int) apth_nthreads;

// Dedicated thread registry
lll_internal_t __dedicated_registry_lock;
struct list __dedicated_registry;

APTH_INTERNAL void apth_dedicated_registry_init(void)
{
    lll_internal_init(&__dedicated_registry_lock);
    list_init(&__dedicated_registry);
}

// Wrapper function for dedicated thread pthreads.
// This is the start_routine passed to pthread_create for dedicated threads.
APTH_INTERNAL void *apth_dedicated_thread_wrapper(void *arg)
{
    apth_t self = (apth_t)arg;

    // Set up TLS so that CUR_SCHED and CUR_APTH macros work
    SET_CUR_SCHED(self->dedicated_dummy_sched);
    self->dedicated_dummy_sched->cur = self;

    // Block SIGPROF — LIBAPTH's preemption timer must not fire on
    // dedicated threads since they have no scheduler to yield to.
    sigset_t ss_block;
    sigemptyset(&ss_block);
    sigaddset(&ss_block, SIGPROF);
    pthread_sigmask(SIG_BLOCK, &ss_block, NULL);

    // Add to dedicated registry for enumeration
    lll_internal_lock(&__dedicated_registry_lock);
    list_push_back(&__dedicated_registry, &self->dedicated_elem);
    lll_internal_unlock(&__dedicated_registry_lock);

    // Mark thread as running
    atomic_store_release(&self->state, APTH_STATE_RUNNING);

    // Run the user function
    void *result = self->start_func(self->start_arg);

    // Exit path for dedicated threads
    apth_dedicated_do_exit(result);

    // For DETACHED threads, clean up resources here since no one will join us
    if (IS_DETACHED(self))
    {
        if (self->dedicated_wake_fd >= 0)
            apth_func_raw(close)(self->dedicated_wake_fd);

        free(self->dedicated_dummy_sched);
        self->magic = 0;
        atomic_decrement_if_positive(&apth_nthreads);
        free(self);
    }

    return NULL;
}

// Exit path for dedicated threads (no scheduler involvement).
// Runs cleanup handlers, TLS destructors, signals joiner, sets TERMINATED.
APTH_INTERNAL void apth_dedicated_do_exit(void *result)
{
    apth_t self = CUR_APTH;

    // Main apth exited
    if (self == get_MAIN_APTH())
        atomic_store_release(&MAIN_APTH_EXITED, 1);

    // Execute cleanup handlers
    apth_thread_cleanup(self);

    // Run TLS destructors (POSIX: after cleanup handlers, before thread dies)
    apth_key_destroydata(self);

    // Store the return value for joiner
    self->join_arg = result;

    // Mark as terminated
    atomic_store_release(&self->state, APTH_STATE_TERMINATED);

    // Decrement alive thread count
    dec_alive_thrcnt();

    // Wake the joiner if one exists
    apth_t joiner = atomic_load_acquire(&self->joinid);
    if (joiner != APTH_NULL && joiner != self && APTH_IS_VALID(joiner))
    {
        if (joiner->is_dedicated)
        {
            // Joiner is also a dedicated thread — wake its pthread via eventfd
            apth_dedicated_unblock(joiner);
        }
        else
        {
            // Joiner is a regular apth — wake its scheduler
            apth_sched_wake(joiner->current_sched);
        }
    }

    // Remove from dedicated registry
    lll_internal_lock(&__dedicated_registry_lock);
    list_remove(&self->dedicated_elem);
    lll_internal_unlock(&__dedicated_registry_lock);

    // Clear current thread — we are done
    SET_CUR_APTH(NULL);
}

// Block a dedicated thread using futex (fast userspace mutex).
// Futex has a fast path: if the value is already non-zero (woken), the
// thread returns immediately without entering the kernel. Only blocks
// (kernel syscall) when the value is zero (no wake pending).
// This replaces eventfd which ALWAYS enters the kernel (read syscall).
APTH_INTERNAL void apth_dedicated_block(apth_t t)
{
    APTH_STAT_INC(__apth_stats_read.eventfd_reads);
    // Spin-check first: if already woken, skip syscall entirely
    while (__atomic_load_n(&t->dedicated_futex_val, __ATOMIC_ACQUIRE) == 0) {
        // Not yet woken — block in kernel until value changes from 0
        syscall(SYS_futex, &t->dedicated_futex_val, FUTEX_WAIT_PRIVATE,
                0 /* expected */, NULL /* no timeout */, NULL, 0);
        // Spurious wakeup possible — loop to recheck
    }
    // Reset for next block
    __atomic_store_n(&t->dedicated_futex_val, 0, __ATOMIC_RELEASE);
}

APTH_INTERNAL void apth_dedicated_unblock(apth_t t)
{
    APTH_STAT_INC(__apth_stats_write.eventfd_writes);
    // Set value to 1 (woken) and wake one waiter
    __atomic_store_n(&t->dedicated_futex_val, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &t->dedicated_futex_val, FUTEX_WAKE_PRIVATE,
            1 /* wake one */, NULL, NULL, 0);
}
