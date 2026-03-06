/*
 * test_signal_wait_apth.c
 *
 * Test sigwait() under libapth.
 *
 * Scenario
 * --------
 *   1. Main apth blocks SIGUSR1 and SIGUSR2.
 *   2. Spawns child A that sends SIGUSR1 after 1s.
 *   3. Spawns child B that sends SIGUSR2 after 2s.
 *   4. Main calls sigwait() twice, expecting SIGUSR1 then SIGUSR2 (order
 *      may vary depending on delivery timing).
 *
 * Pass criteria: sigwait returns both SIGUSR1 and SIGUSR2 exactly once each.
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

static apth_t g_main_th;

static void *sender_func(void *arg)
{
    int sig = (int)(long)arg;
    if (sig == SIGUSR1)
        sleep(1);
    else
        sleep(2);
    apth_kill(g_main_th, sig);
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    g_main_th = apth_self();

    /* Block SIGUSR1 and SIGUSR2 so they become pending for sigwait */
    sigset_t wait_set;
    sigemptyset(&wait_set);
    sigaddset(&wait_set, SIGUSR1);
    sigaddset(&wait_set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &wait_set, NULL);

    /* Spawn senders */
    apth_t child_a, child_b;
    apth_create(&child_a, NULL, sender_func, (void *)(long)SIGUSR1);
    apth_create(&child_b, NULL, sender_func, (void *)(long)SIGUSR2);

    /* Wait for both signals via sigwait */
    int got_usr1 = 0, got_usr2 = 0;
    for (int i = 0; i < 2; i++) {
        int sig;
        int rc = sigwait(&wait_set, &sig);
        if (rc != 0) {
            fprintf(stderr, "[FAIL] test_signal_wait_apth: sigwait returned %d\n", rc);
            exit(1);
        }
        if (sig == SIGUSR1) got_usr1++;
        else if (sig == SIGUSR2) got_usr2++;
        else {
            fprintf(stderr, "[FAIL] test_signal_wait_apth: unexpected signal %d\n", sig);
            exit(1);
        }
    }

    apth_join(child_a, NULL);
    apth_join(child_b, NULL);

    if (got_usr1 == 1 && got_usr2 == 1) {
        fprintf(stderr, "[PASS] test_signal_wait_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_signal_wait_apth: USR1=%d USR2=%d (expected 1,1)\n",
            got_usr1, got_usr2);
    exit(1);
}
APTH_MAIN_END
