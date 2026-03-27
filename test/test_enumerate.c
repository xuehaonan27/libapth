/*
 * test_enumerate.c -- Test apth_for_each_thread and apth_get_worker_count
 *
 * Create 5 regular + 2 dedicated threads, call apth_for_each_thread,
 * verify count >= 7 (may include main thread too).
 */
#include "apth.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

static apth_mutex_t gate_mtx;
static apth_cond_t gate_cond;
static volatile int gate_open = 0;
static _Atomic(int) started = 0;

static void *regular_worker(void *arg)
{
    (void)arg;
    atomic_fetch_add(&started, 1);

    apth_mutex_lock(&gate_mtx);
    while (!gate_open)
        apth_cond_wait(&gate_cond, &gate_mtx);
    apth_mutex_unlock(&gate_mtx);

    return NULL;
}

static void *dedicated_worker(void *arg)
{
    (void)arg;
    atomic_fetch_add(&started, 1);

    apth_mutex_lock(&gate_mtx);
    while (!gate_open)
        apth_cond_wait(&gate_cond, &gate_mtx);
    apth_mutex_unlock(&gate_mtx);

    return NULL;
}

static int counting_visitor(apth_t th, void *arg)
{
    (void)th;
    int *count = (int *)arg;
    (*count)++;
    return 0;
}

#define N_REGULAR 5
#define N_DEDICATED 2

APTH_CONFIG(cfg, cfg->workers = 2;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    apth_mutex_init(&gate_mtx, NULL);
    apth_cond_init(&gate_cond, NULL);

    // Check worker count
    int wc = apth_get_worker_count();
    if (wc != 2)
    {
        static const char msg[] = "FAIL: apth_get_worker_count != 2\n";
        write(2, msg, sizeof(msg) - 1);
        exit(1);
    }

    apth_t regulars[N_REGULAR];
    apth_t dedicated[N_DEDICATED];

    // Create regular threads
    for (int i = 0; i < N_REGULAR; i++)
    {
        if (apth_create(&regulars[i], NULL, regular_worker, NULL) != 0)
        {
            static const char msg[] = "FAIL: apth_create regular\n";
            write(2, msg, sizeof(msg) - 1);
            exit(1);
        }
    }

    // Create dedicated threads
    apth_attr_t dattr;
    apth_attr_init(&dattr);
    apth_attr_setclass_np(&dattr, APTH_CLASS_DEDICATED);
    for (int i = 0; i < N_DEDICATED; i++)
    {
        if (apth_create(&dedicated[i], &dattr, dedicated_worker, NULL) != 0)
        {
            static const char msg[] = "FAIL: apth_create dedicated\n";
            write(2, msg, sizeof(msg) - 1);
            exit(1);
        }
    }

    // Wait for all threads to start
    while (atomic_load(&started) < N_REGULAR + N_DEDICATED)
        apth_yield();

    // Give threads time to settle into WAITING state
    for (int i = 0; i < 200; i++)
        apth_yield();

    // Enumerate
    int visitor_count = 0;
    int total = apth_for_each_thread(counting_visitor, &visitor_count);

    // total includes the main thread + N_REGULAR + N_DEDICATED
    // visitor_count and total should match
    if (total < N_REGULAR + N_DEDICATED)
    {
        static const char msg[] = "FAIL: too few threads enumerated\n";
        write(2, msg, sizeof(msg) - 1);
        exit(1);
    }

    if (visitor_count != total)
    {
        static const char msg[] = "FAIL: visitor count mismatch\n";
        write(2, msg, sizeof(msg) - 1);
        exit(1);
    }

    // Open gate and join all
    apth_mutex_lock(&gate_mtx);
    gate_open = 1;
    apth_cond_broadcast(&gate_cond);
    apth_mutex_unlock(&gate_mtx);

    for (int i = 0; i < N_REGULAR; i++)
        apth_join(regulars[i], NULL);
    for (int i = 0; i < N_DEDICATED; i++)
        apth_join(dedicated[i], NULL);

    apth_cond_destroy(&gate_cond);
    apth_mutex_destroy(&gate_mtx);

    static const char msg[] = "test_enumerate: PASS\n";
    write(2, msg, sizeof(msg) - 1);
    exit(0);
}
APTH_MAIN_END
