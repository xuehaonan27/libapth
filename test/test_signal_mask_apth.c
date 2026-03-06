/*
 * test_signal_mask_apth.c
 *
 * Test pthread_sigmask() / sigprocmask() signal masking under libapth.
 *
 * Scenario
 * --------
 *   1. Main apth blocks SIGUSR1 via sigprocmask(SIG_BLOCK).
 *   2. Registers a handler for SIGUSR1.
 *   3. Child sends SIGUSR1 to main — handler must NOT run (signal is blocked).
 *   4. Main verifies handler has not run.
 *   5. Main unblocks SIGUSR1 — handler should run now (signal was pending).
 *   6. Main verifies handler ran exactly once.
 *
 * Pass criteria: handler runs exactly once, only after unblock.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif // _GNU_SOURCE
#include "apth.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

#define N_WORKERS 1

static _Atomic int g_handler_count = 0;
static apth_t g_main_th;
static _Atomic int g_child_sent = 0;

static void handler_usr1(int sig)
{
    (void)sig;
    atomic_fetch_add(&g_handler_count, 1);
}

static void *child_func(void *arg)
{
    (void)arg;
    /* Wait a bit for main to block the signal */
    sleep(1);

    /* Send SIGUSR1 to main (should be blocked) */
    apth_kill(g_main_th, SIGUSR1);
    atomic_store(&g_child_sent, 1);

    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    g_main_th = apth_self();

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
    sigprocmask(SIG_BLOCK, &block_set, &old_set);

    /* Spawn child that sends SIGUSR1 */
    apth_t child;
    apth_create(&child, NULL, child_func, NULL);

    /* Wait for child to send the signal */
    apth_join(child, NULL);

    /* Yield a few times to give scheduler a chance to (incorrectly) deliver */
    for (int i = 0; i < 5; i++)
        apth_yield();

    /* Verify handler has NOT run (signal is blocked) */
    int count_before = atomic_load(&g_handler_count);
    if (count_before != 0) {
        fprintf(stderr, "[FAIL] test_signal_mask_apth: handler ran %d times while blocked\n",
                count_before);
        exit(1);
    }

    /* Verify signal is pending */
    sigset_t pend;
    sigpending(&pend);
    if (!sigismember(&pend, SIGUSR1)) {
        fprintf(stderr, "[FAIL] test_signal_mask_apth: SIGUSR1 not in pending set\n");
        exit(1);
    }

    /* Unblock SIGUSR1 — pending signal should now be delivered */
    sigprocmask(SIG_UNBLOCK, &block_set, NULL);

    /* Yield to allow delivery */
    apth_yield();
    usleep(100000);

    int count_after = atomic_load(&g_handler_count);
    if (count_after == 1) {
        fprintf(stderr, "[PASS] test_signal_mask_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_signal_mask_apth: handler ran %d times after unblock (expected 1)\n",
            count_after);
    exit(1);
}
APTH_MAIN_END
