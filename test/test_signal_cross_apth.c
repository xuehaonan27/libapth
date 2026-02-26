/*
 * test_signal_cross_apth.c
 *
 * Test cross-thread signal delivery under libapth with 2 workers.
 *
 * Scenario
 * --------
 *   1. Spawn 4 apths on 2 workers, each waiting for a different signal
 *      via sigwait (SIGUSR1, SIGUSR2, SIGRTMIN, SIGRTMIN+1).
 *   2. Main apth sends each signal to the corresponding child via apth_kill.
 *   3. Each child records which signal it received and exits.
 *   4. Main joins all children and verifies correctness.
 *
 * This tests:
 *   - Cross-worker signal delivery (apth_kill to apth on different scheduler)
 *   - sigwait on multiple threads simultaneously
 *   - Signal routing to correct target
 *
 * Pass criteria: each child receives exactly the signal sent to it.
 */
#define _GNU_SOURCE
#include "apth.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sched.h>

#define N_WORKERS   2
#define N_CHILDREN  4

struct child_arg {
    int expected_sig;
    _Atomic int received_sig;
};

static void *child_func(void *arg)
{
    struct child_arg *ca = (struct child_arg *)arg;
    int sig = ca->expected_sig;

    /* Block the signal we want to wait for */
    sigset_t wait_set;
    sigemptyset(&wait_set);
    sigaddset(&wait_set, sig);
    sigprocmask(SIG_BLOCK, &wait_set, NULL);

    /* Wait for it */
    int got_sig;
    int rc = sigwait(&wait_set, &got_sig);
    if (rc != 0) {
        atomic_store(&ca->received_sig, -1);
        return NULL;
    }
    atomic_store(&ca->received_sig, got_sig);
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    int signals[N_CHILDREN] = { SIGUSR1, SIGUSR2, SIGRTMIN, SIGRTMIN + 1 };
    struct child_arg args[N_CHILDREN];
    apth_t children[N_CHILDREN];

    /* Spawn children, alternating affinity across workers */
    for (int i = 0; i < N_CHILDREN; i++) {
        args[i].expected_sig = signals[i];
        atomic_store(&args[i].received_sig, 0);

        apth_attr_t attr;
        apth_attr_init(&attr);

        /* Alternate workers for cross-worker testing */
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i % N_WORKERS, &cpuset);
        apth_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);

        char name[32];
        snprintf(name, sizeof(name), "child_%d_sig%d", i, signals[i]);
        apth_attr_setname_np(&attr, name);

        apth_create(&children[i], &attr, child_func, &args[i]);
        apth_attr_destroy(&attr);
    }

    /* Give children time to enter sigwait */
    sleep(2);

    /* Send each child its expected signal */
    for (int i = 0; i < N_CHILDREN; i++) {
        apth_kill(children[i], signals[i]);
    }

    /* Join all and check results */
    int pass = 1;
    for (int i = 0; i < N_CHILDREN; i++) {
        apth_join(children[i], NULL);
        int got = atomic_load(&args[i].received_sig);
        if (got != signals[i]) {
            fprintf(stderr, "[FAIL] child %d: expected sig %d, got %d\n",
                    i, signals[i], got);
            pass = 0;
        }
    }

    if (pass) {
        fprintf(stderr, "[PASS] test_signal_cross_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_signal_cross_apth\n");
    exit(1);
}
APTH_MAIN_END
