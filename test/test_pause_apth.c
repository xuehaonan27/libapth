/*
 * test_pause_apth.c
 *
 * Test pause() under libapth.
 * pause() suspends execution until a signal is caught.
 */
#define _GNU_SOURCE
#include "apth.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

#define N_WORKERS 2

static _Atomic int g_signal_received = 0;
static apth_t g_paused_thread;

static void handler_usr1(int sig)
{
    (void)sig;
    atomic_store(&g_signal_received, 1);
}

static void *paused_func(void *arg)
{
    (void)arg;

    /* This should block until signal arrives */
    int rv = pause();

    /* pause() always returns -1 with errno EINTR */
    if (rv != -1) {
        fprintf(stderr, "[FAIL] pause() returned %d, expected -1\n", rv);
        exit(1);
    }

    return NULL;
}

static void *signaler_func(void *arg)
{
    (void)arg;

    /* Give paused thread time to call pause() */
    usleep(100000);

    /* Send signal to paused thread */
    apth_kill(g_paused_thread, SIGUSR1);

    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    /* Register signal handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    apth_t signaler;
    apth_create(&g_paused_thread, NULL, paused_func, NULL);
    apth_create(&signaler, NULL, signaler_func, NULL);

    apth_join(g_paused_thread, NULL);
    apth_join(signaler, NULL);

    if (atomic_load(&g_signal_received)) {
        fprintf(stderr, "[PASS] test_pause_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_pause_apth: signal not received\n");
    exit(1);
}
APTH_MAIN_END
