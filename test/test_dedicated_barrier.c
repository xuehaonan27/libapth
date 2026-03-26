#include "apth.h"
#include "utils/apth_string.h"
#include <stdlib.h>
#include <stdint.h>

/*
 * Test: barrier synchronization between dedicated and regular apth threads.
 *
 * Barrier count = 4 (2 dedicated + 2 regular).
 * Each thread sets its flag to 1 before the barrier, then verifies all
 * 4 flags are 1 after the barrier.
 */

#define N_DEDICATED 2
#define N_REGULAR   2
#define N_TOTAL     (N_DEDICATED + N_REGULAR)
#define N_WORKERS   2

static apth_barrier_t barrier;
static volatile int flags[N_TOTAL]; /* indexed 0..3 */
static volatile int post_barrier_ok[N_TOTAL]; /* 1 if thread saw all flags */

static void *
barrier_func(void *arg)
{
    int id = (int)(intptr_t)arg;

    /* Pre-barrier: set own flag */
    flags[id] = 1;

    /* Wait at barrier */
    apth_barrier_wait(&barrier);

    /* Post-barrier: check all flags are set */
    int all_set = 1;
    for (int i = 0; i < N_TOTAL; i++)
    {
        if (flags[i] != 1)
        {
            all_set = 0;
            break;
        }
    }
    post_barrier_ok[id] = all_set;

    return NULL;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char msg[] = "test_dedicated_barrier: starting\n";
    write(2, msg, sizeof(msg) - 1);

    /* Initialize */
    for (int i = 0; i < N_TOTAL; i++)
    {
        flags[i] = 0;
        post_barrier_ok[i] = 0;
    }

    int rc = apth_barrier_init(&barrier, NULL, N_TOTAL);
    if (rc != 0)
    {
        static char err[] = "test_dedicated_barrier: barrier_init failed\n";
        write(2, err, sizeof(err) - 1);
        exit(EXIT_FAILURE);
    }

    apth_t tids[N_TOTAL];
    int idx = 0;

    /* Create 2 dedicated threads (ids 0, 1) */
    for (int i = 0; i < N_DEDICATED; i++)
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
        rc = apth_create(&tids[idx], &attr, barrier_func,
                         (void *)(intptr_t)idx);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] =
                "test_dedicated_barrier: dedicated create failed\n";
            write(2, err, sizeof(err) - 1);
            exit(EXIT_FAILURE);
        }
        idx++;
    }

    /* Create 2 regular threads (ids 2, 3) */
    for (int i = 0; i < N_REGULAR; i++)
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        rc = apth_create(&tids[idx], &attr, barrier_func,
                         (void *)(intptr_t)idx);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] =
                "test_dedicated_barrier: regular create failed\n";
            write(2, err, sizeof(err) - 1);
            exit(EXIT_FAILURE);
        }
        idx++;
    }

    /* Join all */
    for (int i = 0; i < N_TOTAL; i++)
        apth_join(tids[i], NULL);

    /* Verify every thread saw all flags == 1 after the barrier */
    int failures = 0;
    for (int i = 0; i < N_TOTAL; i++)
    {
        if (post_barrier_ok[i] != 1)
        {
            char buf[128];
            int len = apth_snprintf(
                buf, sizeof(buf),
                "test_dedicated_barrier: thread %d did not see all flags\n",
                i);
            write(2, buf, len);
            failures++;
        }
    }

    apth_barrier_destroy(&barrier);

    if (failures == 0)
    {
        static char ok[] = "test_dedicated_barrier: PASS\n";
        write(2, ok, sizeof(ok) - 1);
        exit(0);
    }
    else
    {
        static char fail[] = "test_dedicated_barrier: FAIL\n";
        write(2, fail, sizeof(fail) - 1);
        exit(EXIT_FAILURE);
    }
}
APTH_MAIN_END
