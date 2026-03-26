#include "apth.h"
#include "utils/apth_string.h"
#include <stdlib.h>
#include <stdint.h>

/*
 * Test: shared mutex between dedicated and regular apth threads.
 *
 * 1 dedicated thread + 4 regular apths each lock a shared mutex and
 * increment a counter 1000 times.  Final counter must be 5 * 1000 = 5000.
 */

#define N_REGULAR    4
#define N_INCREMENTS 1000
#define N_WORKERS    2

static apth_mutex_t mtx;
static volatile int shared_counter = 0;

static void *
increment_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < N_INCREMENTS; i++)
    {
        apth_mutex_lock(&mtx);
        shared_counter++;
        apth_mutex_unlock(&mtx);
    }

    return NULL;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char msg[] = "test_dedicated_mutex: starting\n";
    write(2, msg, sizeof(msg) - 1);

    int rc = apth_mutex_init(&mtx, NULL);
    if (rc != 0)
    {
        static char err[] = "test_dedicated_mutex: apth_mutex_init failed\n";
        write(2, err, sizeof(err) - 1);
        exit(EXIT_FAILURE);
    }

    apth_t dedicated_tid;
    apth_t regular_tids[N_REGULAR];

    /* Create the dedicated thread */
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
        rc = apth_create(&dedicated_tid, &attr, increment_func, NULL);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] =
                "test_dedicated_mutex: dedicated create failed\n";
            write(2, err, sizeof(err) - 1);
            exit(EXIT_FAILURE);
        }
    }

    /* Create regular apth threads */
    for (int i = 0; i < N_REGULAR; i++)
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        rc = apth_create(&regular_tids[i], &attr, increment_func,
                         (void *)(intptr_t)i);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] =
                "test_dedicated_mutex: regular create failed\n";
            write(2, err, sizeof(err) - 1);
            exit(EXIT_FAILURE);
        }
    }

    /* Join all threads */
    apth_join(dedicated_tid, NULL);
    for (int i = 0; i < N_REGULAR; i++)
        apth_join(regular_tids[i], NULL);

    /* Verify final counter */
    int expected = (1 + N_REGULAR) * N_INCREMENTS; /* 5 * 1000 = 5000 */
    if (shared_counter != expected)
    {
        char buf[128];
        int len = apth_snprintf(
            buf, sizeof(buf),
            "test_dedicated_mutex: FAIL counter=%d expected=%d\n",
            shared_counter, expected);
        write(2, buf, len);
        exit(EXIT_FAILURE);
    }

    apth_mutex_destroy(&mtx);

    static char ok[] = "test_dedicated_mutex: PASS\n";
    write(2, ok, sizeof(ok) - 1);
    exit(0);
}
APTH_MAIN_END
