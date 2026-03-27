/*
 * test_safepoint.c -- Test apth_request_pause_all / apth_resume_all
 *
 * Uses a dedicated thread as the external coordinator (like a JVM GC thread)
 * that requests pauses and verifies that regular threads stop.
 *
 * Steps:
 *   1. Create 4 regular threads doing a busy increment loop
 *   2. Dedicated coordinator pauses all
 *   3. Verify no regular thread is RUNNING (counter stops increasing)
 *   4. Resume all
 *   5. Wait for threads to finish, verify counter
 */
#include "apth.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sched.h>

#define N_THREADS 4
#define ITERS_PER_THREAD 50000

static _Atomic(int) counter = 0;
static _Atomic(int) go = 0;
static _Atomic(int) test_result = 0; // 0 = not done, 1 = pass, -1 = fail

static void *worker(void *arg)
{
    (void)arg;

    // Wait for the go signal
    while (!atomic_load(&go))
        apth_yield();

    for (int i = 0; i < ITERS_PER_THREAD; i++)
    {
        atomic_fetch_add(&counter, 1);
        // Yield periodically to allow scheduler to cooperate
        if (i % 100 == 0)
            apth_yield();
    }

    return NULL;
}

// Coordinator runs as a dedicated thread so it can call apth_request_pause_all
// without being affected by the pause itself.
static void *coordinator(void *arg)
{
    apth_t *tids = (apth_t *)arg;

    // Let threads start
    atomic_store(&go, 1);

    // Busy-wait to let threads make progress
    for (volatile int i = 0; i < 1000000; i++)
        ;

    // Pause all
    apth_request_pause_all();

    // After pause, counter should stop changing
    int snap1 = atomic_load(&counter);
    // Busy-wait a bit
    for (volatile int i = 0; i < 500000; i++)
        ;
    int snap2 = atomic_load(&counter);

    if (snap1 != snap2)
    {
        atomic_store(&test_result, -1);
        apth_resume_all();
        return NULL;
    }

    // Resume
    apth_resume_all();

    // Join all threads
    for (int i = 0; i < N_THREADS; i++)
        apth_join(tids[i], NULL);

    int final_val = atomic_load(&counter);
    if (final_val == N_THREADS * ITERS_PER_THREAD)
        atomic_store(&test_result, 1);
    else
        atomic_store(&test_result, -1);

    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = 2;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    apth_t tids[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
    {
        if (apth_create(&tids[i], NULL, worker, NULL) != 0)
        {
            static const char msg[] = "FAIL: apth_create\n";
            write(2, msg, sizeof(msg) - 1);
            exit(1);
        }
    }

    // Create dedicated coordinator thread
    apth_attr_t dattr;
    apth_attr_init(&dattr);
    apth_attr_setclass_np(&dattr, APTH_CLASS_DEDICATED);

    apth_t coord;
    if (apth_create(&coord, &dattr, coordinator, tids) != 0)
    {
        static const char msg[] = "FAIL: create coordinator\n";
        write(2, msg, sizeof(msg) - 1);
        exit(1);
    }

    apth_join(coord, NULL);

    int result = atomic_load(&test_result);
    if (result == 1)
    {
        static const char msg[] = "test_safepoint: PASS\n";
        write(2, msg, sizeof(msg) - 1);
    }
    else
    {
        static const char msg[] = "FAIL: safepoint verification failed\n";
        write(2, msg, sizeof(msg) - 1);
        exit(1);
    }

    exit(0);
}
APTH_MAIN_END
