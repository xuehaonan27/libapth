#include "apth.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define N_WORKERS 2
#define N_PRODUCERS 3
#define N_CONSUMERS 3
#define N_ITEMS_PER_PRODUCER 100

// Test 1: Multiple producers and consumers
static apth_sem_t sem_items;
static apth_mutex_t sum_mtx;
static volatile long total_produced = 0;
static volatile long total_consumed = 0;

static void* multi_producer_func(void *arg)
{
    int id = (int)(intptr_t)arg;

    for (int i = 0; i < N_ITEMS_PER_PRODUCER; i++)
    {
        int value = id * 1000 + i;

        apth_mutex_lock(&sum_mtx);
        total_produced += value;
        apth_mutex_unlock(&sum_mtx);

        apth_sem_post(&sem_items);
    }

    return NULL;
}

static void* multi_consumer_func(void *arg)
{
    (void)arg;

    int items_to_consume = (N_PRODUCERS * N_ITEMS_PER_PRODUCER) / N_CONSUMERS;

    for (int i = 0; i < items_to_consume; i++)
    {
        apth_sem_wait(&sem_items);

        // Simulate consuming (we can't get the actual value from semaphore)
        // So we just count
    }

    return NULL;
}

// Test 2: Timed wait
static apth_sem_t timed_sem;

static void* timedwait_timeout_func(void *arg)
{
    (void)arg;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 200000000; // 200ms
    if (ts.tv_nsec >= 1000000000)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    int rc = apth_sem_timedwait(&timed_sem, &ts);
    if (rc != ETIMEDOUT)
    {
        write(2, "ERROR: sem_timedwait should timeout\n", 37);
        exit(EXIT_FAILURE);
    }

    return NULL;
}

static void* timedwait_success_func(void *arg)
{
    (void)arg;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5; // 5 seconds (should not timeout)

    int rc = apth_sem_timedwait(&timed_sem, &ts);
    if (rc == ETIMEDOUT)
    {
        write(2, "ERROR: sem_timedwait should not timeout\n", 41);
        exit(EXIT_FAILURE);
    }

    return NULL;
}

// Test 3: Trywait contention
static apth_sem_t trywait_sem;
static volatile int trywait_successes = 0;
static volatile int trywait_failures = 0;

static void* trywait_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < 50; i++)
    {
        int rc = apth_sem_trywait(&trywait_sem);
        if (rc == 0)
        {
            __sync_fetch_and_add(&trywait_successes, 1);
            // Post it back
            apth_sem_post(&trywait_sem);
        }
        else if (rc == EAGAIN)
        {
            __sync_fetch_and_add(&trywait_failures, 1);
        }
        apth_yield();
    }

    return NULL;
}

// Test 4: Semaphore as counter
static apth_sem_t counter_sem;

static void* counter_poster_func(void *arg)
{
    int count = (int)(intptr_t)arg;

    for (int i = 0; i < count; i++)
    {
        apth_sem_post(&counter_sem);
    }

    return NULL;
}

static void* counter_waiter_func(void *arg)
{
    int count = (int)(intptr_t)arg;

    for (int i = 0; i < count; i++)
    {
        apth_sem_wait(&counter_sem);
    }

    return NULL;
}

// Test 5: Getvalue
static apth_sem_t getvalue_sem;

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    write(2, "test_sem_advanced: starting\n", 28);

    // Test 1: Multiple producers and consumers
    write(2, "  Test 1: Multiple producers/consumers...\n", 43);
    apth_sem_init(&sem_items, 0, 0);
    apth_mutex_init(&sum_mtx, NULL);

    apth_t producers[N_PRODUCERS];
    apth_t consumers[N_CONSUMERS];

    for (int i = 0; i < N_PRODUCERS; i++)
    {
        apth_create(&producers[i], NULL, multi_producer_func, (void*)(intptr_t)i);
    }

    for (int i = 0; i < N_CONSUMERS; i++)
    {
        apth_create(&consumers[i], NULL, multi_consumer_func, NULL);
    }

    for (int i = 0; i < N_PRODUCERS; i++)
    {
        apth_join(producers[i], NULL);
    }

    for (int i = 0; i < N_CONSUMERS; i++)
    {
        apth_join(consumers[i], NULL);
    }

    // Check semaphore is empty
    int val;
    apth_sem_getvalue(&sem_items, &val);
    if (val != 0)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: semaphore value=%d, expected 0\n", val);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_sem_destroy(&sem_items);
    apth_mutex_destroy(&sum_mtx);
    write(2, "    PASS\n", 9);

    // Test 2: Timed wait timeout
    write(2, "  Test 2: Timed wait timeout...\n", 33);
    apth_sem_init(&timed_sem, 0, 0);

    apth_t timeout_thread;
    apth_create(&timeout_thread, NULL, timedwait_timeout_func, NULL);
    apth_join(timeout_thread, NULL);

    apth_sem_destroy(&timed_sem);
    write(2, "    PASS\n", 9);

    // Test 3: Timed wait success
    write(2, "  Test 3: Timed wait success...\n", 33);
    apth_sem_init(&timed_sem, 0, 0);

    apth_t success_thread;
    apth_create(&success_thread, NULL, timedwait_success_func, NULL);

    // Give thread time to start waiting
    usleep(100000); // 100ms

    // Post to wake it up
    apth_sem_post(&timed_sem);

    apth_join(success_thread, NULL);

    apth_sem_destroy(&timed_sem);
    write(2, "    PASS\n", 9);

    // Test 4: Trywait contention
    write(2, "  Test 4: Trywait contention...\n", 33);
    apth_sem_init(&trywait_sem, 0, 5); // Start with 5

    apth_t trywait_threads[8];
    for (int i = 0; i < 8; i++)
    {
        apth_create(&trywait_threads[i], NULL, trywait_func, NULL);
    }

    for (int i = 0; i < 8; i++)
    {
        apth_join(trywait_threads[i], NULL);
    }

    // In userspace threading, we may not get failures due to cooperative scheduling
    // Just verify we got some successes
    if (trywait_successes == 0)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: trywait had no successes\n");
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_sem_destroy(&trywait_sem);
    write(2, "    PASS\n", 9);

    // Test 5: Semaphore as counter
    write(2, "  Test 5: Semaphore as counter...\n", 35);
    apth_sem_init(&counter_sem, 0, 0);

    apth_t poster1, poster2, waiter1, waiter2;
    apth_create(&poster1, NULL, counter_poster_func, (void*)(intptr_t)50);
    apth_create(&poster2, NULL, counter_poster_func, (void*)(intptr_t)30);
    apth_create(&waiter1, NULL, counter_waiter_func, (void*)(intptr_t)40);
    apth_create(&waiter2, NULL, counter_waiter_func, (void*)(intptr_t)40);

    apth_join(poster1, NULL);
    apth_join(poster2, NULL);
    apth_join(waiter1, NULL);
    apth_join(waiter2, NULL);

    // Should have 0 remaining
    apth_sem_getvalue(&counter_sem, &val);
    if (val != 0)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: counter semaphore value=%d, expected 0\n", val);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_sem_destroy(&counter_sem);
    write(2, "    PASS\n", 9);

    // Test 6: Getvalue
    write(2, "  Test 6: Getvalue...\n", 23);
    apth_sem_init(&getvalue_sem, 0, 10);

    apth_sem_getvalue(&getvalue_sem, &val);
    if (val != 10)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: initial value=%d, expected 10\n", val);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_sem_wait(&getvalue_sem);
    apth_sem_wait(&getvalue_sem);
    apth_sem_wait(&getvalue_sem);

    apth_sem_getvalue(&getvalue_sem, &val);
    if (val != 7)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: after 3 waits value=%d, expected 7\n", val);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_sem_post(&getvalue_sem);
    apth_sem_post(&getvalue_sem);

    apth_sem_getvalue(&getvalue_sem, &val);
    if (val != 9)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: after 2 posts value=%d, expected 9\n", val);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_sem_destroy(&getvalue_sem);
    write(2, "    PASS\n", 9);

    write(2, "test_sem_advanced: ALL TESTS PASSED\n", 37);
}
APTH_MAIN_END
