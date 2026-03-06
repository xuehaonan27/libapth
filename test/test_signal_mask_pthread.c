/*
 * test_signal_mask_pthread.c
 *
 * Pthread baseline for test_signal_mask_apth.c.
 * Tests pthread_sigmask() signal masking and pending delivery.
 *
 * Pass criteria: handler runs exactly once, only after unblock.
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

static _Atomic int g_handler_count = 0;
static pthread_t g_main_th;
static _Atomic int g_child_sent = 0;

static void handler_usr1(int sig)
{
    (void)sig;
    atomic_fetch_add(&g_handler_count, 1);
}

static void *child_func(void *arg)
{
    (void)arg;
    sleep(1);
    pthread_kill(g_main_th, SIGUSR1);
    atomic_store(&g_child_sent, 1);
    return NULL;
}

int main(void)
{
    g_main_th = pthread_self();

    /* Register handler for SIGUSR1 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    /* Block SIGUSR1 */
    sigset_t block_set, old_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block_set, &old_set);

    /* Spawn child */
    pthread_t child;
    pthread_create(&child, NULL, child_func, NULL);
    pthread_join(child, NULL);

    usleep(100000);

    /* Verify handler has NOT run */
    int count_before = atomic_load(&g_handler_count);
    if (count_before != 0) {
        fprintf(stderr, "[FAIL] test_signal_mask_pthread: handler ran %d times while blocked\n",
                count_before);
        return 1;
    }

    /* Verify signal is pending */
    sigset_t pend;
    sigpending(&pend);
    if (!sigismember(&pend, SIGUSR1)) {
        fprintf(stderr, "[FAIL] test_signal_mask_pthread: SIGUSR1 not in pending set\n");
        return 1;
    }

    /* Unblock — signal should now be delivered */
    pthread_sigmask(SIG_UNBLOCK, &block_set, NULL);
    usleep(100000);

    int count_after = atomic_load(&g_handler_count);
    if (count_after == 1) {
        fprintf(stderr, "[PASS] test_signal_mask_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_signal_mask_pthread: handler ran %d times after unblock (expected 1)\n",
            count_after);
    return 1;
}
