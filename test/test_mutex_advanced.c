#include "apth.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define N_WORKERS 2
#define N_THREADS 10
#define N_INCREMENTS 500

// Test 1: Recursive mutex
static apth_mutex_t recursive_mtx;
static int recursive_depth = 0;

static void* test_recursive_func(void *arg)
{
    (void)arg;

    // Lock multiple times
    for (int i = 0; i < 5; i++)
    {
        apth_mutex_lock(&recursive_mtx);
        recursive_depth++;
    }

    // Unlock same number of times
    for (int i = 0; i < 5; i++)
    {
        recursive_depth--;
        apth_mutex_unlock(&recursive_mtx);
    }

    return NULL;
}

// Test 2: Error-check mutex
static apth_mutex_t errorcheck_mtx;

static void* test_errorcheck_func(void *arg)
{
    (void)arg;

    apth_mutex_lock(&errorcheck_mtx);

    // Try to lock again - should return EDEADLK
    int rc = apth_mutex_lock(&errorcheck_mtx);
    if (rc != EDEADLK)
    {
        write(2, "ERROR: errorcheck mutex should return EDEADLK\n", 47);
        exit(EXIT_FAILURE);
    }

    apth_mutex_unlock(&errorcheck_mtx);

    // Try to unlock again - should return EPERM
    rc = apth_mutex_unlock(&errorcheck_mtx);
    if (rc != EPERM)
    {
        write(2, "ERROR: errorcheck unlock should return EPERM\n", 46);
        exit(EXIT_FAILURE);
    }

    return NULL;
}

// Test 3: Timed lock
static apth_mutex_t timed_mtx;
static volatile int timed_holder_ready = 0;

static void* timed_holder_func(void *arg)
{
    (void)arg;

    apth_mutex_lock(&timed_mtx);
    timed_holder_ready = 1;

    // Hold for 2 seconds
    sleep(2);

    apth_mutex_unlock(&timed_mtx);
    return NULL;
}

static void* timed_waiter_func(void *arg)
{
    (void)arg;

    // Wait for holder to acquire lock
    while (!timed_holder_ready)
        apth_yield();

    // Try to acquire with 500ms timeout - should fail
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 500000000; // 500ms
    if (ts.tv_nsec >= 1000000000)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    int rc = apth_mutex_timedlock(&timed_mtx, &ts);
    if (rc != ETIMEDOUT)
    {
        write(2, "ERROR: timedlock should timeout\n", 33);
        exit(EXIT_FAILURE);
    }

    return NULL;
}

// Test 4: Trylock contention
static apth_mutex_t trylock_mtx;
static volatile int trylock_successes = 0;
static volatile int trylock_failures = 0;

static void* trylock_contention_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < 100; i++)
    {
        int rc = apth_mutex_trylock(&trylock_mtx);
        if (rc == 0)
        {
            __sync_fetch_and_add(&trylock_successes, 1);
            apth_mutex_unlock(&trylock_mtx);
        }
        else if (rc == EBUSY)
        {
            __sync_fetch_and_add(&trylock_failures, 1);
        }
        apth_yield();
    }

    return NULL;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    write(2, "test_mutex_advanced: starting\n", 30);

    // Test 1: Recursive mutex
    write(2, "  Test 1: Recursive mutex...\n", 30);
    apth_mutexattr_t recursive_attr;
    apth_mutexattr_init(&recursive_attr);
    apth_mutexattr_settype(&recursive_attr, APTH_MUTEX_RECURSIVE);
    apth_mutex_init(&recursive_mtx, &recursive_attr);
    apth_mutexattr_destroy(&recursive_attr);

    apth_t recursive_thread;
    apth_create(&recursive_thread, NULL, test_recursive_func, NULL);
    apth_join(recursive_thread, NULL);

    if (recursive_depth != 0)
    {
        write(2, "ERROR: recursive depth mismatch\n", 33);
        exit(EXIT_FAILURE);
    }
    apth_mutex_destroy(&recursive_mtx);
    write(2, "    PASS\n", 9);

    // Test 2: Error-check mutex
    write(2, "  Test 2: Error-check mutex...\n", 32);
    apth_mutexattr_t errorcheck_attr;
    apth_mutexattr_init(&errorcheck_attr);
    apth_mutexattr_settype(&errorcheck_attr, APTH_MUTEX_ERRORCHECK);
    apth_mutex_init(&errorcheck_mtx, &errorcheck_attr);
    apth_mutexattr_destroy(&errorcheck_attr);

    apth_t errorcheck_thread;
    apth_create(&errorcheck_thread, NULL, test_errorcheck_func, NULL);
    apth_join(errorcheck_thread, NULL);

    apth_mutex_destroy(&errorcheck_mtx);
    write(2, "    PASS\n", 9);

    // Test 3: Timed lock
    write(2, "  Test 3: Timed lock...\n", 25);
    apth_mutex_init(&timed_mtx, NULL);

    apth_t holder, waiter;
    apth_create(&holder, NULL, timed_holder_func, NULL);
    apth_create(&waiter, NULL, timed_waiter_func, NULL);

    apth_join(holder, NULL);
    apth_join(waiter, NULL);

    apth_mutex_destroy(&timed_mtx);
    write(2, "    PASS\n", 9);

    // Test 4: Trylock contention
    write(2, "  Test 4: Trylock contention...\n", 33);
    apth_mutex_init(&trylock_mtx, NULL);

    apth_t trylock_threads[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
    {
        apth_create(&trylock_threads[i], NULL, trylock_contention_func, NULL);
    }

    for (int i = 0; i < N_THREADS; i++)
    {
        apth_join(trylock_threads[i], NULL);
    }

    // In userspace threading, we may not get failures due to cooperative scheduling
    // Just verify we got some successes
    if (trylock_successes == 0)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: trylock had no successes\n");
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_mutex_destroy(&trylock_mtx);
    write(2, "    PASS\n", 9);

    write(2, "test_mutex_advanced: ALL TESTS PASSED\n", 39);
}
APTH_MAIN_END
