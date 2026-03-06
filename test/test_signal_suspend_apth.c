/*
 * test_signal_suspend_apth.c
 *
 * Test sigsuspend() under libapth.
 *
 * Scenario
 * --------
 *   1. Main apth blocks SIGUSR1.
 *   2. Registers handler for SIGUSR1.
 *   3. Spawns child that sends SIGUSR1 after 1s.
 *   4. Main calls sigsuspend() with a mask that does NOT include SIGUSR1
 *      (i.e., temporarily unblocks it). sigsuspend should:
 *      a) atomically replace the signal mask
 *      b) wait until a signal is delivered
 *      c) restore the old mask and return -1 with errno == EINTR
 *   5. Verify handler ran and sigsuspend returned correctly.
 *
 * Pass criteria: handler runs once, sigsuspend returns -1 with EINTR,
 *                original mask is restored after sigsuspend.
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
#include <errno.h>

#define N_WORKERS 1

static _Atomic int g_handler_count = 0;
static apth_t g_main_th;

static void handler_usr1(int sig)
{
    (void)sig;
    atomic_fetch_add(&g_handler_count, 1);
}

static void *child_func(void *arg)
{
    (void)arg;
    sleep(1);
    apth_kill(g_main_th, SIGUSR1);
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;
    int pass = 1;

    g_main_th = apth_self();

    /* Register handler for SIGUSR1 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    /* Block SIGUSR1 */
    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block_set, NULL);

    /* Spawn child */
    apth_t child;
    apth_create(&child, NULL, child_func, NULL);

    /* sigsuspend with empty mask (unblocks SIGUSR1 temporarily) */
    sigset_t suspend_mask;
    sigemptyset(&suspend_mask);
    /* Do NOT add SIGUSR1 to suspend_mask — this means it's unblocked during suspend */

    int rc = sigsuspend(&suspend_mask);
    int saved_errno = errno;

    /* sigsuspend should return -1 with EINTR */
    if (rc != -1 || saved_errno != EINTR) {
        fprintf(stderr, "[FAIL] sigsuspend returned %d, errno=%d (expected -1, EINTR=%d)\n",
                rc, saved_errno, EINTR);
        pass = 0;
    }

    /* Handler should have run once */
    int count = atomic_load(&g_handler_count);
    if (count != 1) {
        fprintf(stderr, "[FAIL] handler ran %d times (expected 1)\n", count);
        pass = 0;
    }

    /* Verify original mask is restored (SIGUSR1 should be blocked again) */
    sigset_t current_mask;
    sigprocmask(SIG_SETMASK, NULL, &current_mask);
    if (!sigismember(&current_mask, SIGUSR1)) {
        fprintf(stderr, "[FAIL] SIGUSR1 not re-blocked after sigsuspend\n");
        pass = 0;
    }

    apth_join(child, NULL);

    if (pass) {
        fprintf(stderr, "[PASS] test_signal_suspend_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_signal_suspend_apth\n");
    exit(1);
}
APTH_MAIN_END
