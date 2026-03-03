#include "apth.h"
#include "utils/apth_string.h"
#include <stdlib.h>
#include <string.h>

#define N_THREADS 50
#define N_INCREMENTS 1000
#define N_WORKERS 2

static apth_mutex_t mtx;
static volatile int shared_counter = 0;

static void *
increment_func(void *arg)
{
    int id = (int)(intptr_t)arg;
    (void)id;

    for (int i = 0; i < N_INCREMENTS; i++)
    {
        apth_mutex_lock(&mtx);
        shared_counter++;
        apth_mutex_unlock(&mtx);
    }

    return (void *)(intptr_t)id;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

static apth_t tids[N_THREADS];

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char msg[] = "test_mutex: starting\n";
    write(2, msg, sizeof(msg) - 1);

    int rc = apth_mutex_init(&mtx, NULL);
    if (rc != 0)
    {
        static char err[] = "test_mutex: apth_mutex_init failed\n";
        write(2, err, sizeof(err) - 1);
        exit(EXIT_FAILURE);
    }

    // Test trylock on free mutex
    rc = apth_mutex_trylock(&mtx);
    if (rc != 0)
    {
        static char err[] = "test_mutex: trylock on free mutex failed\n";
        write(2, err, sizeof(err) - 1);
        exit(EXIT_FAILURE);
    }
    apth_mutex_unlock(&mtx);

    // Spawn threads that all increment a shared counter
    for (int i = 0; i < N_THREADS; i++)
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        apth_create(&tids[i], &attr, increment_func, (void *)(intptr_t)i);
        apth_attr_destroy(&attr);
    }

    for (int i = 0; i < N_THREADS; i++)
    {
        void *retval;
        apth_join(tids[i], &retval);
        if ((int)(intptr_t)retval != i)
        {
            static char err[] = "test_mutex: thread returned wrong value\n";
            write(2, err, sizeof(err) - 1);
            exit(EXIT_FAILURE);
        }
    }

    int expected = N_THREADS * N_INCREMENTS;
    if (shared_counter != expected)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_mutex: FAIL counter=%d expected=%d\n",
                                shared_counter, expected);
        write(2, buf, len);
        exit(EXIT_FAILURE);
    }

    apth_mutex_destroy(&mtx);

    static char ok[] = "test_mutex: PASS\n";
    write(2, ok, sizeof(ok) - 1);
}
APTH_MAIN_END
