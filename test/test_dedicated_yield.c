/*
 * test_dedicated_yield.c -- Test apth_yield() from a dedicated thread
 *
 * A dedicated thread runs on its own pthread.  Calling apth_yield()
 * should not crash or hang (it may be a no-op or fall through to
 * sched_yield).  Also create a regular apth that yields in parallel
 * to verify both coexist without issues.
 */
#include "apth.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

#define N_WORKERS 2
#define YIELD_COUNT 100

static _Atomic(int) dedicated_done = 0;
static _Atomic(int) regular_done = 0;

static void *
dedicated_yield_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < YIELD_COUNT; i++)
        apth_yield();
    atomic_store(&dedicated_done, 1);
    return NULL;
}

static void *
regular_yield_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < YIELD_COUNT; i++)
        apth_yield();
    atomic_store(&regular_done, 1);
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char starting[] = "test_dedicated_yield: starting\n";
    write(2, starting, sizeof(starting) - 1);

    apth_t ded_th, reg_th;

    /* Create the dedicated thread */
    apth_attr_t attr;
    apth_attr_init(&attr);
    apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
    int rc = apth_create(&ded_th, &attr, dedicated_yield_worker, NULL);
    apth_attr_destroy(&attr);
    if (rc != 0)
    {
        static char err[] = "test_dedicated_yield: FAIL (dedicated create failed)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Create a regular apth */
    rc = apth_create(&reg_th, NULL, regular_yield_worker, NULL);
    if (rc != 0)
    {
        static char err[] = "test_dedicated_yield: FAIL (regular create failed)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    apth_join(ded_th, NULL);
    apth_join(reg_th, NULL);

    if (!atomic_load(&dedicated_done))
    {
        static char err[] = "test_dedicated_yield: FAIL (dedicated thread incomplete)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    if (!atomic_load(&regular_done))
    {
        static char err[] = "test_dedicated_yield: FAIL (regular thread incomplete)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    static char ok[] = "test_dedicated_yield: PASS\n";
    write(2, ok, sizeof(ok) - 1);
    exit(0);
}
APTH_MAIN_END
