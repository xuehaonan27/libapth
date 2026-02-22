#include "apth.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/atomic_wrapper.h"
#include "utils/archplattoold.h"

/*
typedef _Atomic(int) apth_once_t;
*/

static void clear_once_control(void *arg)
{
    apth_once_t *once_control = (apth_once_t *)arg;
    atomic_store_relaxed(once_control, 0);
}

static int
    __attribute__((noinline))
    __apth_once_slow(apth_once_t *once_control, void (*init_routine)(void))
{
    // NOTE: this implementation in GNU NPTL considered fork
    // But we do not need to consider that.

    int val, newval;

    // Acquire current value
    val = atomic_load_acquire(once_control);
    do
    {
        // Check if the initializaiton has already been done.
        if (apth_likely((val & __APTH_ONCE_DONE) != 0))
            return 0;

        // Initialization not done. We try to set the state to in-progress and
        // having the current fork generation. We don't need atomic accesses for
        // the fork generation because it is immutable in a particular process,
        // and forked child processes start with a single thread that modified
        // the generation.

        // NOTE: GNU NPTL considers fork, but we do not need it here.
        //    newval = __fork_generation | __PTHREAD_ONCE_INPROGRESS;
        newval = __APTH_ONCE_INPROGRESS;

    } while (apth_unlikely(!atomic_compare_exchange_weak_acquire(once_control, &val, newval)));

    // We acquired permit to perform initialization

    // Push a routine in case the apth got cancelled during initialization
    apth_cleanup_push(clear_once_control, once_control);

    init_routine();

    apth_cleanup_pop(0);

    // Mark *once_control as having finished
    atomic_store_release(once_control, __APTH_ONCE_DONE);
    return 0;
}

int apth_once(apth_once_t *once_control, void (*init_routine)(void))
{
    // Fast path.
    int val;

    val = atomic_load_acquire(once_control);
    if (apth_likely((val & __APTH_ONCE_DONE) != 0))
        return 0;
    else
        return __apth_once_slow(once_control, init_routine);
}