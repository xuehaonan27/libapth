/*
 * test_signal_raise_pthread.c
 *
 * Pthread baseline for test_signal_raise_apth.c.
 * Tests raise() (self-signaling) and SA_RESETHAND.
 *
 * Pass criteria: USR1 handler runs once, USR2 handler runs once,
 *                SA_RESETHAND correctly resets to SIG_DFL.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

static _Atomic int g_usr1_count = 0;
static _Atomic int g_usr2_count = 0;

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

int main(void)
{
    int pass = 1;

    /* --- Test 1: raise(SIGUSR1) with simple handler --- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    raise(SIGUSR1);

    if (atomic_load(&g_usr1_count) != 1) {
        fprintf(stderr, "[FAIL] raise(SIGUSR1): handler ran %d times (expected 1)\n",
                atomic_load(&g_usr1_count));
        pass = 0;
    }

    /* --- Test 2: raise(SIGUSR2) with SA_RESETHAND --- */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr2;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGUSR2, &sa, NULL);

    raise(SIGUSR2);

    if (atomic_load(&g_usr2_count) != 1) {
        fprintf(stderr, "[FAIL] raise(SIGUSR2): handler ran %d times (expected 1)\n",
                atomic_load(&g_usr2_count));
        pass = 0;
    }

    /* After SA_RESETHAND, sigaction should now be SIG_DFL */
    struct sigaction old_sa;
    sigaction(SIGUSR2, NULL, &old_sa);
    if (old_sa.sa_handler != SIG_DFL) {
        fprintf(stderr, "[FAIL] SA_RESETHAND: handler not reset to SIG_DFL\n");
        pass = 0;
    }

    if (pass) {
        fprintf(stderr, "[PASS] test_signal_raise_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_signal_raise_pthread\n");
    return 1;
}
