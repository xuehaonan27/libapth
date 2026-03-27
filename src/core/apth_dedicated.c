#include "common.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_dedicated.h"
#include "internal/apth_cleanup.h"
#include "internal/apth_data.h"
#include "internal/apth_sched.h"
#include "hook_libc/hooked_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/lll.inline.h"
#include "utils/list.inline.h"
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

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

// Block a dedicated thread. Blocks the calling pthread on the thread's
// dedicated_wake_fd (eventfd). Used by sync primitives when a dedicated
// thread must wait.
APTH_INTERNAL void apth_dedicated_block(apth_t t)
{
    uint64_t val;
    apth_func_raw(read)(t->dedicated_wake_fd, &val, sizeof(val));
}

// Unblock a dedicated thread. Writes to the thread's dedicated_wake_fd
// to wake it from apth_dedicated_block(). Safe to call from any context
// (regular apth, scheduler, or another dedicated thread).
APTH_INTERNAL void apth_dedicated_unblock(apth_t t)
{
    uint64_t val = 1;
    apth_func_raw(write)(t->dedicated_wake_fd, &val, sizeof(val));
}
