/*
 * test_detach.c — Test apth_detach()
 *
 * Verify that apth_join returns EINVAL after detach, and that
 * the detached thread runs to completion.
 */
#include "apth.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>

static _Atomic(int) thread_completed = 0;

static void *detached_worker(void *arg)
{
    (void)arg;
    /* Do some work */
    for (volatile int i = 0; i < 10000; i++)
        ;
    atomic_store(&thread_completed, 1);
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = 2;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    apth_t th;
    apth_create(&th, NULL, detached_worker, NULL);

    /* Detach the thread */
    int rc = apth_detach(th);
    if (rc != 0)
    {
        char buf[128];
        int len = snprintf(buf, sizeof(buf),
                           "test_detach: FAIL (apth_detach returned %d)\n", rc);
        write(2, buf, len);
        exit(1);
    }

    /* Join should fail with EINVAL on a detached thread */
    rc = apth_join(th, NULL);
    if (rc != EINVAL)
    {
        char buf[128];
        int len = snprintf(buf, sizeof(buf),
                           "test_detach: FAIL (apth_join returned %d, expected EINVAL=%d)\n",
                           rc, EINVAL);
        write(2, buf, len);
        exit(1);
    }

    /* Wait for detached thread to complete */
    for (int i = 0; i < 1000; i++)
    {
        if (atomic_load(&thread_completed))
            break;
        usleep(1000); /* 1ms */
    }

    if (!atomic_load(&thread_completed))
    {
        write(2, "test_detach: FAIL (detached thread did not complete)\n", 52);
        exit(1);
    }

    write(2, "test_detach: PASS\n", 18);
    exit(0);
}
APTH_MAIN_END
