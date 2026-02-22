#include "apth_state_transition.h"
#include "debug.h"
#include "atomic_wrapper.h"
#include "../internal_types.h"
#include <errno.h>
#include <assert.h>

/**
 * Execute an atomic state transition.
 * 
 * This function ensures atomicity by:
 * 1. Acquiring necessary locks (from_queue and/or to_queue)
 * 2. Verifying the thread is in the expected state
 * 3. Removing from source queue (if any)
 * 4. Updating thread state
 * 5. Adding to destination queue (if any)
 * 6. Releasing locks
 * 
 * For cross-scheduler operations, locks are acquired in a consistent order
 * to prevent deadlocks (lower address first).
 */
APTH_INTERNAL int apth_atomic_state_transition(apth_state_transition_t *trans)
{
    assert(trans != NULL);
    assert(trans->thread != NULL);
    assert(APTH_IS_VALID(trans->thread));
    
    apth_t th = trans->thread;
    int result = 0;
    
    // Determine which locks we need
    bool need_from_lock = (trans->from_queue != NULL);
    bool need_to_lock = (trans->to_queue != NULL);
    bool same_queue = (trans->from_queue == trans->to_queue);
    
    // For cross-scheduler or when we need locks, acquire them in order
    if (trans->cross_scheduler || need_from_lock || need_to_lock) {
        // Acquire locks in consistent order to prevent deadlock
        apth_runqueue_t *first_lock = NULL;
        apth_runqueue_t *second_lock = NULL;
        
        if (need_from_lock && need_to_lock && !same_queue) {
            // Need both locks, determine order
            if (trans->from_queue < trans->to_queue) {
                first_lock = trans->from_queue;
                second_lock = trans->to_queue;
            } else {
                first_lock = trans->to_queue;
                second_lock = trans->from_queue;
            }
            
            apth_rq_lock(first_lock, "apth_atomic_state_transition_1");
            apth_rq_lock(second_lock, "apth_atomic_state_transition_2");
        } else if (need_from_lock) {
            apth_rq_lock(trans->from_queue, "apth_atomic_state_transition_from");
        } else if (need_to_lock) {
            apth_rq_lock(trans->to_queue, "apth_atomic_state_transition_to");
        }
    }
    
    // Verify state if required
    if (!trans->skip_state_check) {
        if (th->state != trans->from_state) {
            result = -EINVAL;
            goto unlock_and_return;
        }
    }
    
    // Remove from source queue if present
    if (trans->from_queue != NULL) {
        if (!apth_rq_remove(trans->from_queue, th)) {
            // Thread not in expected queue
            result = -ESRCH;
            goto unlock_and_return;
        }
    }
    
    // Update state atomically
    th->state = trans->to_state;
    
    // Add to destination queue if present
    if (trans->to_queue != NULL) {
        result = apth_rq_enqueue(trans->to_queue, th);
        if (result != 0) {
            // Failed to enqueue, this is bad - try to restore
            // (though this might fail too)
            th->state = trans->from_state;
            if (trans->from_queue != NULL) {
                apth_rq_enqueue(trans->from_queue, th);
            }
            goto unlock_and_return;
        }
    } else {
        // No destination queue means thread is not in any queue
        // Clear belonging information
        th->belongs_to_list = NULL;
        th->belongs_to_list_lock = NULL;
    }
    
unlock_and_return:
    // Release locks in reverse order
    if (trans->cross_scheduler || need_from_lock || need_to_lock) {
        if (need_from_lock && need_to_lock && !same_queue) {
            apth_runqueue_t *first_lock, *second_lock;
            if (trans->from_queue < trans->to_queue) {
                first_lock = trans->from_queue;
                second_lock = trans->to_queue;
            } else {
                first_lock = trans->to_queue;
                second_lock = trans->from_queue;
            }
            apth_rq_unlock(second_lock, "apth_atomic_state_transition_2");
            apth_rq_unlock(first_lock, "apth_atomic_state_transition_1");
        } else if (need_from_lock) {
            apth_rq_unlock(trans->from_queue, "apth_atomic_state_transition_from");
        } else if (need_to_lock) {
            apth_rq_unlock(trans->to_queue, "apth_atomic_state_transition_to");
        }
    }
    
    return result;
}

// ============================================================================
// Helper Functions for Common State Transitions
// ============================================================================

APTH_INTERNAL int apth_transition_new_to_ready(apth_t th, apth_runqueue_t *from_new, apth_runqueue_t *to_ready)
{
    apth_state_transition_t trans = {
        .thread = th,
        .from_state = APTH_STATE_NEW,
        .to_state = APTH_STATE_READY,
        .from_queue = from_new,
        .to_queue = to_ready,
        .cross_scheduler = false,
        .skip_state_check = false
    };
    return apth_atomic_state_transition(&trans);
}

APTH_INTERNAL int apth_transition_ready_to_running(apth_t th, apth_runqueue_t *from_ready)
{
    // Running state means thread is not in any queue
    apth_state_transition_t trans = {
        .thread = th,
        .from_state = APTH_STATE_READY,
        .to_state = APTH_STATE_READY,  // State doesn't change to "RUNNING" in current design
        .from_queue = from_ready,
        .to_queue = NULL,  // Thread leaves all queues when running
        .cross_scheduler = false,
        .skip_state_check = false
    };
    return apth_atomic_state_transition(&trans);
}

APTH_INTERNAL int apth_transition_running_to_ready(apth_t th, apth_runqueue_t *to_ready)
{
    // Thread is not in any queue when running
    apth_state_transition_t trans = {
        .thread = th,
        .from_state = APTH_STATE_READY,  // Technically already READY in current design
        .to_state = APTH_STATE_READY,
        .from_queue = NULL,
        .to_queue = to_ready,
        .cross_scheduler = false,
        .skip_state_check = true  // Skip since thread is executing
    };
    return apth_atomic_state_transition(&trans);
}

APTH_INTERNAL int apth_transition_running_to_waiting(apth_t th, apth_runqueue_t *to_waiting)
{
    apth_state_transition_t trans = {
        .thread = th,
        .from_state = APTH_STATE_READY,  // Current state before waiting
        .to_state = APTH_STATE_WAITING,
        .from_queue = NULL,  // Thread is running, not in queue
        .to_queue = to_waiting,
        .cross_scheduler = false,
        .skip_state_check = true  // Skip since thread is executing
    };
    return apth_atomic_state_transition(&trans);
}

APTH_INTERNAL int apth_transition_waiting_to_ready(apth_t th, apth_runqueue_t *from_waiting, apth_runqueue_t *to_ready)
{
    apth_state_transition_t trans = {
        .thread = th,
        .from_state = APTH_STATE_WAITING,
        .to_state = APTH_STATE_READY,
        .from_queue = from_waiting,
        .to_queue = to_ready,
        .cross_scheduler = false,
        .skip_state_check = false
    };
    return apth_atomic_state_transition(&trans);
}

APTH_INTERNAL int apth_transition_to_terminated(apth_t th, apth_runqueue_t *from_queue, apth_runqueue_t *to_terminated)
{
    apth_state_transition_t trans = {
        .thread = th,
        .from_state = th->state,  // Can terminate from various states
        .to_state = APTH_STATE_TERMINATED,
        .from_queue = from_queue,
        .to_queue = to_terminated,
        .cross_scheduler = false,
        .skip_state_check = true  // Don't check from_state since it can vary
    };
    return apth_atomic_state_transition(&trans);
}

/**
 * Wait for a thread to be placed in a queue after state transition.
 * This replaces the busy-wait in wait_apth_to_be_in_list.
 */
APTH_INTERNAL void apth_wait_for_queue_placement(apth_t th)
{
    // Wait until thread has valid belonging information
    // Use atomic loads with proper memory ordering
    while (atomic_load_explicit(&th->belongs_to_list, memory_order_acquire) == NULL) {
        // Yield to avoid busy-waiting
        sched_yield();
    }
    while (atomic_load_explicit(&th->belongs_to_list_lock, memory_order_acquire) == NULL) {
        sched_yield();
    }
}
