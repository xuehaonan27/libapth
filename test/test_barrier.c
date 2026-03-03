#include "apth.h"
#include "utils/apth_string.h"
#include <stdlib.h>

#define N_THREADS 10
#define N_WORKERS 2

static apth_barrier_t barrier;
static volatile int serial_count = 0;
static apth_mutex_t count_mtx;

static void *
barrier_func(void *arg)
{
    (void)arg;

    int rc = apth_barrier_wait(&barrier);
    if (rc == APTH_BARRIER_SERIAL_THREAD)
    {
        apth_mutex_lock(&count_mtx);
        serial_count++;
        apth_mutex_unlock(&count_mtx);
    }

    return (void *)(intptr_t)rc;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char msg[] = "test_barrier: starting\n";
    write(2, msg, sizeof(msg) - 1);

    apth_mutex_init(&count_mtx, NULL);
    int rc = apth_barrier_init(&barrier, NULL, N_THREADS);
    if (rc != 0)
    {
        static char err[] = "test_barrier: init failed\n";
        write(2, err, sizeof(err) - 1);
        exit(EXIT_FAILURE);
    }

    apth_t tids[N_THREADS];

    for (int i = 0; i < N_THREADS; i++)
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        apth_create(&tids[i], &attr, barrier_func, (void *)(intptr_t)i);
        apth_attr_destroy(&attr);
    }

    for (int i = 0; i < N_THREADS; i++)
        apth_join(tids[i], NULL);

    int failures = 0;

    // Exactly one thread should have received APTH_BARRIER_SERIAL_THREAD
    if (serial_count != 1)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_barrier: serial_count=%d expected=1\n",
                                serial_count);
        write(2, buf, len);
        failures++;
    }

    apth_barrier_destroy(&barrier);
    apth_mutex_destroy(&count_mtx);

    if (failures == 0)
    {
        static char ok[] = "test_barrier: PASS\n";
        write(2, ok, sizeof(ok) - 1);
    }
    else
    {
        static char fail[] = "test_barrier: FAIL\n";
        write(2, fail, sizeof(fail) - 1);
        exit(EXIT_FAILURE);
    }
}
APTH_MAIN_END
