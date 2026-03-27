/*
 * test_stats.c -- Test apth_get_thread_stats()
 *
 * Verify:
 *   1. dispatches > 0 after thread runs
 *   2. cpu_time_sec > 0 after a busy loop
 *   3. wall_time_sec > 0 after thread finishes
 *   4. thread_class matches the class set at creation
 *   5. NULL stats pointer returns EINVAL
 *   6. NULL thread returns ESRCH
 */
#include "apth.h"
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

static volatile int sink = 0;

static void *
busy_worker(void *arg)
{
    (void)arg;

    /* Busy loop to accumulate CPU time */
    for (int i = 0; i < 1000000; i++)
        sink += i;

    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = 2;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    apth_t th;
    apth_attr_t attr;
    apth_attr_init(&attr);
    apth_attr_setclass_np(&attr, APTH_CLASS_CPU_BOUND);

    apth_create(&th, &attr, busy_worker, NULL);
    apth_attr_destroy(&attr);
    apth_join(th, NULL);

    struct apth_thread_stats stats;
    int rc = apth_get_thread_stats(th, &stats);
    if (rc != 0)
    {
        static char err[] = "test_stats: FAIL (apth_get_thread_stats failed)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    if (stats.dispatches <= 0)
    {
        static char err[] = "test_stats: FAIL (dispatches <= 0)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    if (stats.cpu_time_sec <= 0.0)
    {
        static char err[] = "test_stats: FAIL (cpu_time_sec <= 0)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    if (stats.wall_time_sec <= 0.0)
    {
        static char err[] = "test_stats: FAIL (wall_time_sec <= 0)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    if (stats.thread_class != APTH_CLASS_CPU_BOUND)
    {
        static char err[] = "test_stats: FAIL (thread_class != CPU_BOUND)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    if (stats.state != APTH_THREAD_STATE_TERMINATED)
    {
        static char err[] = "test_stats: FAIL (state != TERMINATED)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* NULL stats pointer should return EINVAL */
    rc = apth_get_thread_stats(th, NULL);
    if (rc != EINVAL)
    {
        static char err[] = "test_stats: FAIL (NULL stats should return EINVAL)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* NULL thread should return ESRCH */
    rc = apth_get_thread_stats(NULL, &stats);
    if (rc != ESRCH)
    {
        static char err[] = "test_stats: FAIL (NULL thread should return ESRCH)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    static char ok[] = "test_stats: PASS\n";
    write(2, ok, sizeof(ok) - 1);
    exit(0);
}
APTH_MAIN_END
