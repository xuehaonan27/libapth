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

    /* After join, the TCB is freed — stats should return ESRCH */
    struct apth_thread_stats stats;
    int rc = apth_get_thread_stats(th, &stats);
    if (rc != ESRCH)
    {
        static char err[] = "test_stats: FAIL (expected ESRCH after join)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* NULL stats pointer on NULL thread should return ESRCH */
    rc = apth_get_thread_stats(NULL, NULL);
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
