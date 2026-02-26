/*
 * test_signal_raise_apth.c
 *
 * Test raise() (self-signaling) under libapth.
 *
 * Scenario
 * --------
 *   1. Register a handler for SIGUSR1.
 *   2. Call raise(SIGUSR1) — handler should execute.
 *   3. Register a handler for SIGUSR2 via sigaction with SA_RESETHAND.
 *   4. Call raise(SIGUSR2) — handler should execute once.
 *   5. Call raise(SIGUSR2) again — now SIG_DFL applies, but since we block
 *      the signal first it should just pend (we don't want default term).
 *
 * Pass criteria: USR1 handler runs once, USR2 handler runs once,
 *                SA_RESETHAND correctly resets to SIG_DFL.
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

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;
    int pass = 1;

    /* --- Test 1: raise(SIGUSR1) with simple handler --- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    raise(SIGUSR1);

    /* Yield to give scheduler a chance to deliver */
    apth_yield();

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
    apth_yield();

    if (atomic_load(&g_usr2_count) != 1) {
        fprintf(stderr, "[FAIL] raise(SIGUSR2): handler ran %d times (expected 1)\n",
                atomic_load(&g_usr2_count));
        pass = 0;
    }

    /* After SA_RESETHAND, sigaction should now be SIG_DFL.
       Verify by checking the old action via sigaction(). */
    struct sigaction old_sa;
    sigaction(SIGUSR2, NULL, &old_sa);
    if (old_sa.sa_handler != SIG_DFL) {
        fprintf(stderr, "[FAIL] SA_RESETHAND: handler not reset to SIG_DFL\n");
        pass = 0;
    }

    if (pass) {
        fprintf(stderr, "[PASS] test_signal_raise_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_signal_raise_apth\n");
    exit(1);
}
APTH_MAIN_END
