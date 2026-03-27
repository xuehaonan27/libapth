/*
 * test_mixed_lock_stress.c -- Mixed dedicated + regular thread lock stress
 *
 * 4 dedicated + 8 regular threads, 2 workers.
 * Shared mutex, 10000 lock/increment/unlock cycles each.
 * Shared cond var for signaling between dedicated and regular.
 * Verify counter == 12 * 10000.
 */
#include "apth.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

#define N_DEDICATED 4
#define N_REGULAR   8
#define ITERS       10000

static apth_mutex_t mtx;
static apth_cond_t cond;
static volatile int counter = 0;
static _Atomic(int) phase2_ready = 0;
static _Atomic(int) all_done = 0;

static void *lock_worker(void *arg)
{
    (void)arg;

    for (int i = 0; i < ITERS; i++)
    {
        apth_mutex_lock(&mtx);
        counter++;
        apth_mutex_unlock(&mtx);
    }

    // Signal phase 2 via cond
    atomic_fetch_add(&phase2_ready, 1);

    // Signal the cond to wake anyone waiting
    apth_mutex_lock(&mtx);
    apth_cond_broadcast(&cond);
    apth_mutex_unlock(&mtx);

    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = 2;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    apth_mutex_init(&mtx, NULL);
    apth_cond_init(&cond, NULL);

    apth_t dedicated[N_DEDICATED];
    apth_t regular[N_REGULAR];

    // Create dedicated threads
    apth_attr_t dattr;
    apth_attr_init(&dattr);
    apth_attr_setclass_np(&dattr, APTH_CLASS_DEDICATED);

    for (int i = 0; i < N_DEDICATED; i++)
    {
        if (apth_create(&dedicated[i], &dattr, lock_worker, NULL) != 0)
        {
            static const char msg[] = "FAIL: create dedicated\n";
            write(2, msg, sizeof(msg) - 1);
            exit(1);
        }
    }

    // Create regular threads
    for (int i = 0; i < N_REGULAR; i++)
    {
        if (apth_create(&regular[i], NULL, lock_worker, NULL) != 0)
        {
            static const char msg[] = "FAIL: create regular\n";
            write(2, msg, sizeof(msg) - 1);
            exit(1);
        }
    }

    // Wait for all threads to signal they are done with their increments
    apth_mutex_lock(&mtx);
    while (atomic_load(&phase2_ready) < N_DEDICATED + N_REGULAR)
        apth_cond_wait(&cond, &mtx);
    apth_mutex_unlock(&mtx);

    // Join all threads
    for (int i = 0; i < N_DEDICATED; i++)
        apth_join(dedicated[i], NULL);
    for (int i = 0; i < N_REGULAR; i++)
        apth_join(regular[i], NULL);

    int expected = (N_DEDICATED + N_REGULAR) * ITERS;
    if (counter == expected)
    {
        static const char msg[] = "test_mixed_lock_stress: PASS\n";
        write(2, msg, sizeof(msg) - 1);
    }
    else
    {
        static const char msg[] = "FAIL: counter mismatch\n";
        write(2, msg, sizeof(msg) - 1);
        exit(1);
    }

    apth_cond_destroy(&cond);
    apth_mutex_destroy(&mtx);

    exit(0);
}
APTH_MAIN_END
