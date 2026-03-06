/*
 * test_signal_cross_pthread.c
 *
 * Pthread baseline for test_signal_cross_apth.c.
 * Tests cross-thread signal delivery with pthread_kill + sigwait.
 *
 * Pass criteria: each child receives exactly the signal sent to it.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif // _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

#define N_CHILDREN 4

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
    pthread_sigmask(SIG_BLOCK, &wait_set, NULL);

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

int main(void)
{
    int signals[N_CHILDREN] = { SIGUSR1, SIGUSR2, SIGRTMIN, SIGRTMIN + 1 };
    struct child_arg args[N_CHILDREN];
    pthread_t children[N_CHILDREN];

    /* Block all test signals in main thread so children inherit the mask */
    sigset_t block_all;
    sigemptyset(&block_all);
    for (int i = 0; i < N_CHILDREN; i++)
        sigaddset(&block_all, signals[i]);
    pthread_sigmask(SIG_BLOCK, &block_all, NULL);

    for (int i = 0; i < N_CHILDREN; i++) {
        args[i].expected_sig = signals[i];
        atomic_store(&args[i].received_sig, 0);
        pthread_create(&children[i], NULL, child_func, &args[i]);
    }

    /* Give children time to enter sigwait */
    sleep(2);

    /* Send each child its expected signal */
    for (int i = 0; i < N_CHILDREN; i++) {
        pthread_kill(children[i], signals[i]);
    }

    /* Join all and check results */
    int pass = 1;
    for (int i = 0; i < N_CHILDREN; i++) {
        pthread_join(children[i], NULL);
        int got = atomic_load(&args[i].received_sig);
        if (got != signals[i]) {
            fprintf(stderr, "[FAIL] child %d: expected sig %d, got %d\n",
                    i, signals[i], got);
            pass = 0;
        }
    }

    if (pass) {
        fprintf(stderr, "[PASS] test_signal_cross_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_signal_cross_pthread\n");
    return 1;
}
