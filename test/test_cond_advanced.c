#include "apth.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define N_WORKERS 2
#define N_THREADS 8

// Test 1: Broadcast wakes all waiters
static apth_mutex_t broadcast_mtx;
static apth_cond_t broadcast_cv;
static volatile int broadcast_ready = 0;
static volatile int broadcast_woken = 0;

static void* broadcast_waiter_func(void *arg)
{
    (void)arg;

    apth_mutex_lock(&broadcast_mtx);
    while (!broadcast_ready)
    {
        apth_cond_wait(&broadcast_cv, &broadcast_mtx);
    }
    __sync_fetch_and_add(&broadcast_woken, 1);
    apth_mutex_unlock(&broadcast_mtx);

    return NULL;
}

// Test 2: Signal wakes one waiter at a time
static apth_mutex_t signal_mtx;
static apth_cond_t signal_cv;
static volatile int signal_count = 0;
static volatile int signal_woken = 0;

static void* signal_waiter_func(void *arg)
{
    (void)arg;

    apth_mutex_lock(&signal_mtx);
    while (signal_count == 0)
    {
        apth_cond_wait(&signal_cv, &signal_mtx);
    }
    signal_count--;
    __sync_fetch_and_add(&signal_woken, 1);
    apth_mutex_unlock(&signal_mtx);

    return NULL;
}

// Test 3: Timed wait timeout
static apth_mutex_t timed_mtx;
static apth_cond_t timed_cv;

static void* timedwait_func(void *arg)
{
    (void)arg;

    apth_mutex_lock(&timed_mtx);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 200000000; // 200ms
    if (ts.tv_nsec >= 1000000000)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    int rc = apth_cond_timedwait(&timed_cv, &timed_mtx, &ts);
    if (rc != ETIMEDOUT)
    {
        write(2, "ERROR: timedwait should timeout\n", 33);
        exit(EXIT_FAILURE);
    }

    apth_mutex_unlock(&timed_mtx);
    return NULL;
}

// Test 4: Timed wait with signal before timeout
static apth_mutex_t timed_signal_mtx;
static apth_cond_t timed_signal_cv;
static volatile int timed_signal_ready = 0;

static void* timedwait_signal_waiter_func(void *arg)
{
    (void)arg;

    apth_mutex_lock(&timed_signal_mtx);
    timed_signal_ready = 1;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5; // 5 seconds (should not timeout)

    int rc = apth_cond_timedwait(&timed_signal_cv, &timed_signal_mtx, &ts);
    if (rc == ETIMEDOUT)
    {
        write(2, "ERROR: timedwait should not timeout\n", 37);
        exit(EXIT_FAILURE);
    }

    apth_mutex_unlock(&timed_signal_mtx);
    return NULL;
}

static void* timedwait_signal_signaler_func(void *arg)
{
    (void)arg;

    // Wait for waiter to be ready
    while (!timed_signal_ready)
        apth_yield();

    // Small delay to ensure waiter is waiting
    usleep(100000); // 100ms

    apth_mutex_lock(&timed_signal_mtx);
    apth_cond_signal(&timed_signal_cv);
    apth_mutex_unlock(&timed_signal_mtx);

    return NULL;
}

// Test 5: Producer-consumer with condvar
#define QUEUE_SIZE 5
#define N_ITEMS 50

static apth_mutex_t queue_mtx;
static apth_cond_t queue_not_empty;
static apth_cond_t queue_not_full;
static int queue[QUEUE_SIZE];
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;

static void* producer_func(void *arg)
{
    (void)arg;

    for (int i = 1; i <= N_ITEMS; i++)
    {
        apth_mutex_lock(&queue_mtx);

        while (queue_count == QUEUE_SIZE)
        {
            apth_cond_wait(&queue_not_full, &queue_mtx);
        }

        queue[queue_tail] = i;
        queue_tail = (queue_tail + 1) % QUEUE_SIZE;
        queue_count++;

        apth_cond_signal(&queue_not_empty);
        apth_mutex_unlock(&queue_mtx);
    }

    return NULL;
}

static volatile long consumer_sum = 0;

static void* consumer_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < N_ITEMS; i++)
    {
        apth_mutex_lock(&queue_mtx);

        while (queue_count == 0)
        {
            apth_cond_wait(&queue_not_empty, &queue_mtx);
        }

        int item = queue[queue_head];
        queue_head = (queue_head + 1) % QUEUE_SIZE;
        queue_count--;

        apth_cond_signal(&queue_not_full);
        apth_mutex_unlock(&queue_mtx);

        consumer_sum += item;
    }

    return NULL;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    write(2, "test_cond_advanced: starting\n", 29);

    // Test 1: Broadcast
    write(2, "  Test 1: Broadcast wakes all...\n", 34);
    apth_mutex_init(&broadcast_mtx, NULL);
    apth_cond_init(&broadcast_cv, NULL);

    apth_t broadcast_threads[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
    {
        apth_create(&broadcast_threads[i], NULL, broadcast_waiter_func, NULL);
    }

    // Give threads time to wait
    usleep(100000); // 100ms

    apth_mutex_lock(&broadcast_mtx);
    broadcast_ready = 1;
    apth_cond_broadcast(&broadcast_cv);
    apth_mutex_unlock(&broadcast_mtx);

    for (int i = 0; i < N_THREADS; i++)
    {
        apth_join(broadcast_threads[i], NULL);
    }

    if (broadcast_woken != N_THREADS)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: broadcast woke %d threads, expected %d\n",
                broadcast_woken, N_THREADS);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_cond_destroy(&broadcast_cv);
    apth_mutex_destroy(&broadcast_mtx);
    write(2, "    PASS\n", 9);

    // Test 2: Signal
    write(2, "  Test 2: Signal wakes one at a time...\n", 41);
    apth_mutex_init(&signal_mtx, NULL);
    apth_cond_init(&signal_cv, NULL);

    apth_t signal_threads[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
    {
        apth_create(&signal_threads[i], NULL, signal_waiter_func, NULL);
    }

    // Give threads time to wait
    usleep(100000); // 100ms

    // Signal each thread one by one
    for (int i = 0; i < N_THREADS; i++)
    {
        apth_mutex_lock(&signal_mtx);
        signal_count++;
        apth_cond_signal(&signal_cv);
        apth_mutex_unlock(&signal_mtx);
        usleep(10000); // 10ms between signals
    }

    for (int i = 0; i < N_THREADS; i++)
    {
        apth_join(signal_threads[i], NULL);
    }

    if (signal_woken != N_THREADS)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: signal woke %d threads, expected %d\n",
                signal_woken, N_THREADS);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_cond_destroy(&signal_cv);
    apth_mutex_destroy(&signal_mtx);
    write(2, "    PASS\n", 9);

    // Test 3: Timed wait timeout
    write(2, "  Test 3: Timed wait timeout...\n", 33);
    apth_mutex_init(&timed_mtx, NULL);
    apth_cond_init(&timed_cv, NULL);

    apth_t timed_thread;
    apth_create(&timed_thread, NULL, timedwait_func, NULL);
    apth_join(timed_thread, NULL);

    apth_cond_destroy(&timed_cv);
    apth_mutex_destroy(&timed_mtx);
    write(2, "    PASS\n", 9);

    // Test 4: Timed wait with signal
    write(2, "  Test 4: Timed wait with signal...\n", 37);
    apth_mutex_init(&timed_signal_mtx, NULL);
    apth_cond_init(&timed_signal_cv, NULL);

    apth_t waiter, signaler;
    apth_create(&waiter, NULL, timedwait_signal_waiter_func, NULL);
    apth_create(&signaler, NULL, timedwait_signal_signaler_func, NULL);

    apth_join(waiter, NULL);
    apth_join(signaler, NULL);

    apth_cond_destroy(&timed_signal_cv);
    apth_mutex_destroy(&timed_signal_mtx);
    write(2, "    PASS\n", 9);

    // Test 5: Producer-consumer
    write(2, "  Test 5: Producer-consumer...\n", 32);
    apth_mutex_init(&queue_mtx, NULL);
    apth_cond_init(&queue_not_empty, NULL);
    apth_cond_init(&queue_not_full, NULL);

    apth_t producer, consumer;
    apth_create(&producer, NULL, producer_func, NULL);
    apth_create(&consumer, NULL, consumer_func, NULL);

    apth_join(producer, NULL);
    apth_join(consumer, NULL);

    long expected = (long)N_ITEMS * (N_ITEMS + 1) / 2;
    if (consumer_sum != expected)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: consumer sum=%ld, expected=%ld\n",
                consumer_sum, expected);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_cond_destroy(&queue_not_full);
    apth_cond_destroy(&queue_not_empty);
    apth_mutex_destroy(&queue_mtx);
    write(2, "    PASS\n", 9);

    write(2, "test_cond_advanced: ALL TESTS PASSED\n", 38);
}
APTH_MAIN_END
