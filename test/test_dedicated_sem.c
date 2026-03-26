#include "apth.h"
#include "utils/apth_string.h"
#include <stdlib.h>
#include <stdint.h>

/*
 * Test: semaphore shared between dedicated and regular apth threads.
 *
 * Semaphore with value 2 (allows at most 2 concurrent holders).
 * 1 dedicated thread + 3 regular apths compete for the semaphore.
 * Each thread: sem_wait, increment active_count, verify active_count <= 2,
 * do brief work (loop), decrement active_count, sem_post.
 * Repeat 200 iterations per thread.
 *
 * PASS if active_count never exceeds 2.
 */

#define N_REGULAR      3
#define N_TOTAL        (1 + N_REGULAR) /* 1 dedicated + 3 regular */
#define SEM_SLOTS      2
#define N_ITERATIONS   200
#define WORK_LOOPS     50
#define N_WORKERS      2

static apth_sem_t sem;
static apth_mutex_t count_mtx;
static volatile int active_count = 0;
static volatile int max_active   = 0;
static volatile int violation    = 0;

static void *
sem_worker(void *arg)
{
    (void)arg;

    for (int i = 0; i < N_ITERATIONS; i++)
    {
        apth_sem_wait(&sem);

        /* Enter critical region: increment active count */
        apth_mutex_lock(&count_mtx);
        active_count++;
        if (active_count > max_active)
            max_active = active_count;
        if (active_count > SEM_SLOTS)
            violation = 1;
        apth_mutex_unlock(&count_mtx);

        /* Brief work */
        volatile int dummy = 0;
        for (int j = 0; j < WORK_LOOPS; j++)
            dummy += j;
        (void)dummy;

        /* Leave critical region: decrement active count */
        apth_mutex_lock(&count_mtx);
        active_count--;
        apth_mutex_unlock(&count_mtx);

        apth_sem_post(&sem);
    }

    return NULL;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char msg[] = "test_dedicated_sem: starting\n";
    write(2, msg, sizeof(msg) - 1);

    apth_sem_init(&sem, 0, SEM_SLOTS);
    apth_mutex_init(&count_mtx, NULL);

    apth_t tids[N_TOTAL];
    int idx = 0;
    int rc;

    /* Create 1 dedicated thread */
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
        rc = apth_create(&tids[idx], &attr, sem_worker, NULL);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] =
                "test_dedicated_sem: dedicated create failed\n";
            write(2, err, sizeof(err) - 1);
            exit(EXIT_FAILURE);
        }
        idx++;
    }

    /* Create 3 regular threads */
    for (int i = 0; i < N_REGULAR; i++)
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        rc = apth_create(&tids[idx], &attr, sem_worker, NULL);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] =
                "test_dedicated_sem: regular create failed\n";
            write(2, err, sizeof(err) - 1);
            exit(EXIT_FAILURE);
        }
        idx++;
    }

    /* Join all */
    for (int i = 0; i < N_TOTAL; i++)
        apth_join(tids[i], NULL);

    int failures = 0;

    if (violation)
    {
        char buf[128];
        int len = apth_snprintf(
            buf, sizeof(buf),
            "test_dedicated_sem: FAIL active_count exceeded %d (max=%d)\n",
            SEM_SLOTS, max_active);
        write(2, buf, len);
        failures++;
    }

    apth_sem_destroy(&sem);
    apth_mutex_destroy(&count_mtx);

    if (failures == 0)
    {
        static char ok[] = "test_dedicated_sem: PASS\n";
        write(2, ok, sizeof(ok) - 1);
        exit(0);
    }
    else
    {
        static char fail[] = "test_dedicated_sem: FAIL\n";
        write(2, fail, sizeof(fail) - 1);
        exit(EXIT_FAILURE);
    }
}
APTH_MAIN_END
