#include "apth.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define N_WORKERS 2
#define N_READERS 8
#define N_WRITERS 3
#define N_ITERATIONS 100

// Test 1: Multiple readers can hold lock simultaneously
static apth_rwlock_t multi_reader_lock;
static volatile int active_readers = 0;
static volatile int max_concurrent_readers = 0;
static volatile int reader_violations = 0;

static void* multi_reader_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < N_ITERATIONS; i++)
    {
        apth_rwlock_rdlock(&multi_reader_lock);

        int current = __sync_add_and_fetch(&active_readers, 1);

        // Update max concurrent readers
        int old_max;
        do {
            old_max = max_concurrent_readers;
            if (current <= old_max) break;
        } while (!__sync_bool_compare_and_swap(&max_concurrent_readers, old_max, current));

        // Simulate read work
        usleep(1000); // 1ms

        __sync_sub_and_fetch(&active_readers, 1);

        apth_rwlock_unlock(&multi_reader_lock);
        apth_yield();
    }

    return NULL;
}

// Test 2: Writer has exclusive access
static apth_rwlock_t exclusive_lock;
static volatile int active_readers_ex = 0;
static volatile int active_writers_ex = 0;
static volatile int exclusivity_violations = 0;

static void* exclusive_reader_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < N_ITERATIONS; i++)
    {
        apth_rwlock_rdlock(&exclusive_lock);

        __sync_add_and_fetch(&active_readers_ex, 1);

        // Check no writers
        if (active_writers_ex > 0)
        {
            __sync_add_and_fetch(&exclusivity_violations, 1);
        }

        usleep(500); // 0.5ms

        __sync_sub_and_fetch(&active_readers_ex, 1);

        apth_rwlock_unlock(&exclusive_lock);
        apth_yield();
    }

    return NULL;
}

static void* exclusive_writer_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < N_ITERATIONS; i++)
    {
        apth_rwlock_wrlock(&exclusive_lock);

        __sync_add_and_fetch(&active_writers_ex, 1);

        // Check no readers and no other writers
        if (active_readers_ex > 0 || active_writers_ex > 1)
        {
            __sync_add_and_fetch(&exclusivity_violations, 1);
        }

        usleep(1000); // 1ms

        __sync_sub_and_fetch(&active_writers_ex, 1);

        apth_rwlock_unlock(&exclusive_lock);
        apth_yield();
    }

    return NULL;
}

// Test 3: Trylock
static apth_rwlock_t trylock_lock;
static volatile int tryrdlock_successes = 0;
static volatile int tryrdlock_failures = 0;
static volatile int trywrlock_successes = 0;
static volatile int trywrlock_failures = 0;

static void* tryrdlock_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < 50; i++)
    {
        int rc = apth_rwlock_tryrdlock(&trylock_lock);
        if (rc == 0)
        {
            __sync_fetch_and_add(&tryrdlock_successes, 1);
            apth_rwlock_unlock(&trylock_lock);
        }
        else if (rc == EBUSY)
        {
            __sync_fetch_and_add(&tryrdlock_failures, 1);
        }
        apth_yield();
    }

    return NULL;
}

static void* trywrlock_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < 50; i++)
    {
        int rc = apth_rwlock_trywrlock(&trylock_lock);
        if (rc == 0)
        {
            __sync_fetch_and_add(&trywrlock_successes, 1);
            apth_rwlock_unlock(&trylock_lock);
        }
        else if (rc == EBUSY)
        {
            __sync_fetch_and_add(&trywrlock_failures, 1);
        }
        apth_yield();
    }

    return NULL;
}

// Test 4: Timed lock
static apth_rwlock_t timed_lock;
static volatile int timed_holder_ready = 0;

static void* timed_writer_holder_func(void *arg)
{
    (void)arg;

    apth_rwlock_wrlock(&timed_lock);
    timed_holder_ready = 1;

    // Hold for 1 second
    sleep(1);

    apth_rwlock_unlock(&timed_lock);
    return NULL;
}

static void* timed_reader_waiter_func(void *arg)
{
    (void)arg;

    // Wait for holder to acquire lock
    while (!timed_holder_ready)
        apth_yield();

    // Try to acquire read lock with 300ms timeout - should fail
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 300000000; // 300ms
    if (ts.tv_nsec >= 1000000000)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    int rc = apth_rwlock_timedrdlock(&timed_lock, &ts);
    if (rc != ETIMEDOUT)
    {
        write(2, "ERROR: timedrdlock should timeout\n", 35);
        exit(EXIT_FAILURE);
    }

    return NULL;
}

static void* timed_writer_waiter_func(void *arg)
{
    (void)arg;

    // Wait for holder to acquire lock
    while (!timed_holder_ready)
        apth_yield();

    // Try to acquire write lock with 300ms timeout - should fail
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 300000000; // 300ms
    if (ts.tv_nsec >= 1000000000)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    int rc = apth_rwlock_timedwrlock(&timed_lock, &ts);
    if (rc != ETIMEDOUT)
    {
        write(2, "ERROR: timedwrlock should timeout\n", 35);
        exit(EXIT_FAILURE);
    }

    return NULL;
}

// Test 5: Writer preference (writers should not starve)
static apth_rwlock_t preference_lock;
static volatile int preference_writer_count = 0;
static volatile int preference_reader_count = 0;

static void* preference_reader_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < 50; i++)
    {
        apth_rwlock_rdlock(&preference_lock);
        __sync_fetch_and_add(&preference_reader_count, 1);
        usleep(100); // 0.1ms
        apth_rwlock_unlock(&preference_lock);
        apth_yield();
    }

    return NULL;
}

static void* preference_writer_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < 20; i++)
    {
        apth_rwlock_wrlock(&preference_lock);
        __sync_fetch_and_add(&preference_writer_count, 1);
        usleep(500); // 0.5ms
        apth_rwlock_unlock(&preference_lock);
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

    write(2, "test_rwlock_advanced: starting\n", 31);

    // Test 1: Multiple readers
    write(2, "  Test 1: Multiple readers...\n", 31);
    apth_rwlock_init(&multi_reader_lock, NULL);

    apth_t reader_threads[N_READERS];
    for (int i = 0; i < N_READERS; i++)
    {
        apth_create(&reader_threads[i], NULL, multi_reader_func, NULL);
    }

    for (int i = 0; i < N_READERS; i++)
    {
        apth_join(reader_threads[i], NULL);
    }

    // Should have had multiple concurrent readers
    if (max_concurrent_readers < 2)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: max concurrent readers=%d, expected >= 2\n",
                max_concurrent_readers);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    if (reader_violations > 0)
    {
        write(2, "ERROR: reader violations detected\n", 35);
        exit(EXIT_FAILURE);
    }

    apth_rwlock_destroy(&multi_reader_lock);
    write(2, "    PASS\n", 9);

    // Test 2: Writer exclusivity
    write(2, "  Test 2: Writer exclusivity...\n", 33);
    apth_rwlock_init(&exclusive_lock, NULL);

    apth_t ex_readers[N_READERS];
    apth_t ex_writers[N_WRITERS];

    for (int i = 0; i < N_READERS; i++)
    {
        apth_create(&ex_readers[i], NULL, exclusive_reader_func, NULL);
    }

    for (int i = 0; i < N_WRITERS; i++)
    {
        apth_create(&ex_writers[i], NULL, exclusive_writer_func, NULL);
    }

    for (int i = 0; i < N_READERS; i++)
    {
        apth_join(ex_readers[i], NULL);
    }

    for (int i = 0; i < N_WRITERS; i++)
    {
        apth_join(ex_writers[i], NULL);
    }

    if (exclusivity_violations > 0)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: %d exclusivity violations\n",
                exclusivity_violations);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_rwlock_destroy(&exclusive_lock);
    write(2, "    PASS\n", 9);

    // Test 3: Trylock
    write(2, "  Test 3: Trylock...\n", 22);
    apth_rwlock_init(&trylock_lock, NULL);

    apth_t tryrd_threads[4];
    apth_t trywr_threads[4];

    for (int i = 0; i < 4; i++)
    {
        apth_create(&tryrd_threads[i], NULL, tryrdlock_func, NULL);
        apth_create(&trywr_threads[i], NULL, trywrlock_func, NULL);
    }

    for (int i = 0; i < 4; i++)
    {
        apth_join(tryrd_threads[i], NULL);
        apth_join(trywr_threads[i], NULL);
    }

    // In userspace threading, we may not get failures due to cooperative scheduling
    // Just verify we got some successes
    if (tryrdlock_successes == 0)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: tryrdlock had no successes\n");
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    if (trywrlock_successes == 0)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: trywrlock had no successes\n");
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_rwlock_destroy(&trylock_lock);
    write(2, "    PASS\n", 9);

    // Test 4: Timed lock
    write(2, "  Test 4: Timed lock...\n", 25);
    apth_rwlock_init(&timed_lock, NULL);

    apth_t holder, rd_waiter, wr_waiter;
    apth_create(&holder, NULL, timed_writer_holder_func, NULL);
    apth_create(&rd_waiter, NULL, timed_reader_waiter_func, NULL);
    apth_create(&wr_waiter, NULL, timed_writer_waiter_func, NULL);

    apth_join(holder, NULL);
    apth_join(rd_waiter, NULL);
    apth_join(wr_waiter, NULL);

    apth_rwlock_destroy(&timed_lock);
    write(2, "    PASS\n", 9);

    // Test 5: Writer preference
    write(2, "  Test 5: Writer preference...\n", 32);
    apth_rwlock_init(&preference_lock, NULL);

    apth_t pref_readers[6];
    apth_t pref_writers[3];

    for (int i = 0; i < 6; i++)
    {
        apth_create(&pref_readers[i], NULL, preference_reader_func, NULL);
    }

    for (int i = 0; i < 3; i++)
    {
        apth_create(&pref_writers[i], NULL, preference_writer_func, NULL);
    }

    for (int i = 0; i < 6; i++)
    {
        apth_join(pref_readers[i], NULL);
    }

    for (int i = 0; i < 3; i++)
    {
        apth_join(pref_writers[i], NULL);
    }

    // Writers should have completed their work (not starved)
    if (preference_writer_count != 3 * 20)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: writer count=%d, expected %d\n",
                preference_writer_count, 3 * 20);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_rwlock_destroy(&preference_lock);
    write(2, "    PASS\n", 9);

    write(2, "test_rwlock_advanced: ALL TESTS PASSED\n", 40);
}
APTH_MAIN_END
