/*
 * test_signal_handler_apth.c
 *
 * Test sigaction() / signal() handler registration and delivery under libapth.
 *
 * Scenario
 * --------
 *   1. Main apth registers a handler for SIGUSR1 via sigaction().
 *   2. Spawns a child apth that sends SIGUSR1 to main via apth_kill().
 *   3. Main verifies the handler ran (via a global flag).
 *   4. Main registers a handler for SIGUSR2 via signal().
 *   5. Child sends SIGUSR2 to main.
 *   6. Main verifies both handlers ran.
 *
 * Pass criteria: both signal handlers execute exactly once.
 */
#define _GNU_SOURCE
#include "apth.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

#define N_WORKERS 1

static _Atomic int g_usr1_count = 0;
static _Atomic int g_usr2_count = 0;
static apth_t g_main_th;

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
    /* Give main a chance to set up handlers */
    sleep(1);

    /* Send SIGUSR1 */
    apth_kill(g_main_th, SIGUSR1);

    /* Small delay, then SIGUSR2 */
    sleep(1);
    apth_kill(g_main_th, SIGUSR2);

    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    g_main_th = apth_self();

    /* Register SIGUSR1 via sigaction */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        fprintf(stderr, "[FAIL] sigaction(SIGUSR1) failed\n");
        exit(1);
    }

    /* Register SIGUSR2 via signal() */
    if (signal(SIGUSR2, handler_usr2) == SIG_ERR) {
        fprintf(stderr, "[FAIL] signal(SIGUSR2) failed\n");
        exit(1);
    }

    apth_t child;
    apth_create(&child, NULL, child_func, NULL);

    /* Wait for child to finish sending signals */
    apth_join(child, NULL);

    /* Allow scheduler to deliver pending signals */
    apth_yield();
    sleep(1);

    int u1 = atomic_load(&g_usr1_count);
    int u2 = atomic_load(&g_usr2_count);

    if (u1 == 1 && u2 == 1) {
        fprintf(stderr, "[PASS] test_signal_handler_apth (USR1=%d, USR2=%d)\n", u1, u2);
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_signal_handler_apth (USR1=%d expected 1, USR2=%d expected 1)\n", u1, u2);
    exit(1);
}
APTH_MAIN_END
