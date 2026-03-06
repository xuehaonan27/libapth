/*
 * test_signal_wait_pthread.c
 *
 * Pthread baseline for test_signal_wait_apth.c.
 * Tests sigwait() with two signals sent from child threads.
 *
 * Pass criteria: sigwait returns both SIGUSR1 and SIGUSR2 exactly once each.
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

static pthread_t g_main_th;

static void *sender_func(void *arg)
{
    int sig = (int)(long)arg;
    if (sig == SIGUSR1)
        sleep(1);
    else
        sleep(2);
    pthread_kill(g_main_th, sig);
    return NULL;
}

int main(void)
{
    g_main_th = pthread_self();

    /* Block SIGUSR1 and SIGUSR2 */
    sigset_t wait_set;
    sigemptyset(&wait_set);
    sigaddset(&wait_set, SIGUSR1);
    sigaddset(&wait_set, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &wait_set, NULL);

    /* Spawn senders */
    pthread_t child_a, child_b;
    pthread_create(&child_a, NULL, sender_func, (void *)(long)SIGUSR1);
    pthread_create(&child_b, NULL, sender_func, (void *)(long)SIGUSR2);

    /* Wait for both signals via sigwait */
    int got_usr1 = 0, got_usr2 = 0;
    for (int i = 0; i < 2; i++) {
        int sig;
        int rc = sigwait(&wait_set, &sig);
        if (rc != 0) {
            fprintf(stderr, "[FAIL] test_signal_wait_pthread: sigwait returned %d\n", rc);
            return 1;
        }
        if (sig == SIGUSR1) got_usr1++;
        else if (sig == SIGUSR2) got_usr2++;
        else {
            fprintf(stderr, "[FAIL] test_signal_wait_pthread: unexpected signal %d\n", sig);
            return 1;
        }
    }

    pthread_join(child_a, NULL);
    pthread_join(child_b, NULL);

    if (got_usr1 == 1 && got_usr2 == 1) {
        fprintf(stderr, "[PASS] test_signal_wait_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_signal_wait_pthread: USR1=%d USR2=%d (expected 1,1)\n",
            got_usr1, got_usr2);
    return 1;
}
