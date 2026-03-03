#include "apth.h"
#include "utils/apth_string.h"
#include <stdlib.h>

#define N_READERS 8
#define N_WRITERS 2
#define N_ITERATIONS 100
#define N_WORKERS 2

static apth_rwlock_t rwl;
static volatile int shared_data = 0;
static volatile int reader_failures = 0;
static apth_mutex_t fail_mtx;

static void *
reader_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < N_ITERATIONS; i++)
    {
        apth_rwlock_rdlock(&rwl);

        // Read shared_data twice and check consistency
        int val1 = shared_data;
        // Small busy loop to give writers a chance to interleave
        volatile int x = 0;
        for (int j = 0; j < 100; j++)
            x += j;
        (void)x;
        int val2 = shared_data;

        if (val1 != val2)
        {
            // Writer interleaved during our read — indicates broken exclusion
            apth_mutex_lock(&fail_mtx);
            reader_failures++;
            apth_mutex_unlock(&fail_mtx);
        }

        apth_rwlock_unlock(&rwl);
    }

    return NULL;
}

static void *
writer_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < N_ITERATIONS; i++)
    {
        apth_rwlock_wrlock(&rwl);
        shared_data++;
        apth_rwlock_unlock(&rwl);
    }

    return NULL;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char msg[] = "test_rwlock: starting\n";
    write(2, msg, sizeof(msg) - 1);

    apth_rwlock_init(&rwl, NULL);
    apth_mutex_init(&fail_mtx, NULL);

    // Test tryrdlock / trywrlock on free lock
    int rc = apth_rwlock_tryrdlock(&rwl);
    if (rc != 0)
    {
        static char err[] = "test_rwlock: tryrdlock failed on free lock\n";
        write(2, err, sizeof(err) - 1);
        exit(EXIT_FAILURE);
    }
    apth_rwlock_unlock(&rwl);

    rc = apth_rwlock_trywrlock(&rwl);
    if (rc != 0)
    {
        static char err[] = "test_rwlock: trywrlock failed on free lock\n";
        write(2, err, sizeof(err) - 1);
        exit(EXIT_FAILURE);
    }
    apth_rwlock_unlock(&rwl);

    apth_t readers[N_READERS];
    apth_t writers[N_WRITERS];

    apth_attr_t attr;
    apth_attr_init(&attr);
    apth_attr_setstacksize(&attr, 4096);

    for (int i = 0; i < N_READERS; i++)
        apth_create(&readers[i], &attr, reader_func, (void *)(intptr_t)i);
    for (int i = 0; i < N_WRITERS; i++)
        apth_create(&writers[i], &attr, writer_func, (void *)(intptr_t)i);

    apth_attr_destroy(&attr);

    for (int i = 0; i < N_WRITERS; i++)
        apth_join(writers[i], NULL);
    for (int i = 0; i < N_READERS; i++)
        apth_join(readers[i], NULL);

    int failures = 0;

    // All writers should have completed: shared_data == N_WRITERS * N_ITERATIONS
    int expected = N_WRITERS * N_ITERATIONS;
    if (shared_data != expected)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_rwlock: data=%d expected=%d\n",
                                shared_data, expected);
        write(2, buf, len);
        failures++;
    }

    if (reader_failures != 0)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_rwlock: reader_failures=%d\n",
                                reader_failures);
        write(2, buf, len);
        failures++;
    }

    apth_rwlock_destroy(&rwl);
    apth_mutex_destroy(&fail_mtx);

    if (failures == 0)
    {
        static char ok[] = "test_rwlock: PASS\n";
        write(2, ok, sizeof(ok) - 1);
    }
    else
    {
        static char fail[] = "test_rwlock: FAIL\n";
        write(2, fail, sizeof(fail) - 1);
        exit(EXIT_FAILURE);
    }
}
APTH_MAIN_END
