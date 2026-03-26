/*
 * test_dedicated_detach.c -- Test detached dedicated threads
 *
 * Verify:
 *   1. A detached dedicated thread runs to completion and sets a flag.
 *   2. apth_join returns EINVAL for a detached dedicated thread.
 */
#include "apth.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>

#define N_WORKERS 2

static _Atomic(int) thread_completed = 0;

static void *
detached_dedicated_worker(void *arg)
{
    (void)arg;
    /* Do some work */
    for (volatile int i = 0; i < 10000; i++)
        ;
    atomic_store(&thread_completed, 1);
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char starting[] = "test_dedicated_detach: starting\n";
    write(2, starting, sizeof(starting) - 1);

    apth_t th;
    apth_attr_t attr;
    apth_attr_init(&attr);
    apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
    int rc = apth_create(&th, &attr, detached_dedicated_worker, NULL);
    apth_attr_destroy(&attr);
    if (rc != 0)
    {
        static char err[] = "test_dedicated_detach: FAIL (apth_create failed)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Detach the dedicated thread */
    rc = apth_detach(th);
    if (rc != 0)
    {
        static char err[] = "test_dedicated_detach: FAIL (apth_detach failed)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Join should fail with EINVAL on a detached thread */
    rc = apth_join(th, NULL);
    if (rc != EINVAL)
    {
        static char err[] = "test_dedicated_detach: FAIL (join did not return EINVAL)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Wait for the detached thread to complete */
    for (int i = 0; i < 1000; i++)
    {
        if (atomic_load(&thread_completed))
            break;
        usleep(1000); /* 1ms */
    }

    if (!atomic_load(&thread_completed))
    {
        static char err[] = "test_dedicated_detach: FAIL (thread did not complete)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    static char ok[] = "test_dedicated_detach: PASS\n";
    write(2, ok, sizeof(ok) - 1);
    exit(0);
}
APTH_MAIN_END
