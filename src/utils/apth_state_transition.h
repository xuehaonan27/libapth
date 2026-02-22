#ifndef __LIBAPTH_UTILS_STATE_TRANSITION_H
#define __LIBAPTH_UTILS_STATE_TRANSITION_H

#include "utils/archplattoold.h"
#include "apth_runqueue.h"
#include <stdbool.h>
// Need full internal_types.h for apth_state_t
#include "../internal_types.h"
// Forward declarations

struct apth_runqueue_st;
typedef struct apth_runqueue_st apth_runqueue_t;

// Atomic state transition descriptor
typedef struct {
    apth_t thread;                    // Thread to transition
    apth_state_t from_state;          // Expected current state
    apth_state_t to_state;            // Target state
    apth_runqueue_t *from_queue;      // Source queue (can be NULL for NEW)
    apth_runqueue_t *to_queue;        // Destination queue (can be NULL for TERMINATED+DETACHED)
    bool cross_scheduler;             // Whether this is a cross-scheduler operation
    bool skip_state_check;            // Skip state verification (for initial creation)
} apth_state_transition_t;

// Execute an atomic state transition
// Returns 0 on success, negative errno on failure
APTH_INTERNAL int apth_atomic_state_transition(apth_state_transition_t *trans);

// Helper functions for common state transitions
APTH_INTERNAL int apth_transition_new_to_ready(apth_t th, apth_runqueue_t *from_new, apth_runqueue_t *to_ready);
APTH_INTERNAL int apth_transition_ready_to_running(apth_t th, apth_runqueue_t *from_ready);
APTH_INTERNAL int apth_transition_running_to_ready(apth_t th, apth_runqueue_t *to_ready);
APTH_INTERNAL int apth_transition_running_to_waiting(apth_t th, apth_runqueue_t *to_waiting);
APTH_INTERNAL int apth_transition_waiting_to_ready(apth_t th, apth_runqueue_t *from_waiting, apth_runqueue_t *to_ready);
APTH_INTERNAL int apth_transition_to_terminated(apth_t th, apth_runqueue_t *from_queue, apth_runqueue_t *to_terminated);

// Wait for a thread to complete its state transition and be placed in a queue
APTH_INTERNAL void apth_wait_for_queue_placement(apth_t th);

#endif // __LIBAPTH_UTILS_STATE_TRANSITION_H
