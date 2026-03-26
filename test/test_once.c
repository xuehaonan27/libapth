/*
 * test_once.c — Test apth_once() one-time initialization
 *
 * Multiple threads call apth_once with the same control variable.
 * Verify the init routine runs exactly once.
 */
#include "apth.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

#define N_THREADS 50

static apth_once_t once_control = 0;
static _Atomic(int) init_count = 0;
static _Atomic(int) init_done = 0;

static void init_routine(void)
{
    atomic_fetch_add(&init_count, 1);
    /* Simulate slow initialization */
    usleep(1000);
    atomic_store(&init_done, 1);
}

static void *worker(void *arg)
{
    (void)arg;
    apth_once(&once_control, init_routine);

    /* Verify init completed */
    if (!atomic_load(&init_done))
    {
        write(2, "test_once: FAIL (init not complete after apth_once returned)\n", 61);
        exit(1);
    }
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = 4;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    apth_t threads[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
        apth_create(&threads[i], NULL, worker, NULL);

    for (int i = 0; i < N_THREADS; i++)
        apth_join(threads[i], NULL);

    int count = atomic_load(&init_count);
    if (count != 1)
    {
        char buf[128];
        int len = snprintf(buf, sizeof(buf),
                           "test_once: FAIL (init ran %d times, expected 1)\n", count);
        write(2, buf, len);
        exit(1);
    }

    write(2, "test_once: PASS\n", 16);
    exit(0);
}
APTH_MAIN_END
