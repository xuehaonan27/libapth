#include "apth.h"
#include "apth_once.h"
#include "utils/atomic_wrapper.h"
#include "utils/archplattoold.h"

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
    for (;;)
    {
        // Check if the initialization has already been done.
        if ((val & __APTH_ONCE_DONE) != 0)
            return 0;

        // If another thread is currently running the init, wait for it.
        if ((val & __APTH_ONCE_INPROGRESS) != 0)
        {
            apth_yield();
            val = atomic_load_acquire(once_control);
            continue;
        }

        // Try to claim the init: CAS 0 → INPROGRESS
        newval = __APTH_ONCE_INPROGRESS;
        if (atomic_compare_exchange_weak_acquire(once_control, &val, newval))
            break; // We won the race, proceed to init
        // CAS failed, val updated by CAS — loop back to recheck
    }

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