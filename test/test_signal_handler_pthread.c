/*
 * test_signal_handler_pthread.c
 *
 * Pthread baseline for test_signal_handler_apth.c.
 * Tests sigaction() / signal() handler registration and delivery.
 *
 * Pass criteria: both signal handlers execute exactly once.
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

static _Atomic int g_usr1_count = 0;
static _Atomic int g_usr2_count = 0;
static pthread_t g_main_th;

static void handler_usr1(int sig)
{
    (void)sig;
    atomic_fetch_add(&g_usr1_count, 1);
}

static void handler_usr2(int sig)
{
    (void)sig;
    atomic_fetch_add(&g_usr2_count, 1);
}

static void *child_func(void *arg)
{
    (void)arg;
    sleep(1);
    pthread_kill(g_main_th, SIGUSR1);
    sleep(1);
    pthread_kill(g_main_th, SIGUSR2);
    return NULL;
}

int main(void)
{
    g_main_th = pthread_self();

    /* Register SIGUSR1 via sigaction */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        fprintf(stderr, "[FAIL] sigaction(SIGUSR1) failed\n");
        return 1;
    }

    /* Register SIGUSR2 via signal() */
    if (signal(SIGUSR2, handler_usr2) == SIG_ERR) {
        fprintf(stderr, "[FAIL] signal(SIGUSR2) failed\n");
        return 1;
    }

    pthread_t child;
    pthread_create(&child, NULL, child_func, NULL);
    pthread_join(child, NULL);

    /* Small delay to let signals be delivered */
    usleep(100000);

    int u1 = atomic_load(&g_usr1_count);
    int u2 = atomic_load(&g_usr2_count);

    if (u1 == 1 && u2 == 1) {
        fprintf(stderr, "[PASS] test_signal_handler_pthread (USR1=%d, USR2=%d)\n", u1, u2);
        return 0;
    }
    fprintf(stderr, "[FAIL] test_signal_handler_pthread (USR1=%d expected 1, USR2=%d expected 1)\n", u1, u2);
    return 1;
}
