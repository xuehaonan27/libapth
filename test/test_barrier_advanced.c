#include "apth.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#define N_WORKERS 2
#define N_THREADS 8
#define N_ROUNDS 5

// Test 1: Basic barrier with multiple rounds
static apth_barrier_t basic_barrier;
static volatile int round_counters[N_ROUNDS] = {0};
static volatile int serial_thread_count = 0;

static void* basic_barrier_func(void *arg)
{
    int id = (int)(intptr_t)arg;
    (void)id;

    for (int round = 0; round < N_ROUNDS; round++)
    {
        // Do some work
        __sync_fetch_and_add(&round_counters[round], 1);

        // Wait at barrier
        int rc = apth_barrier_wait(&basic_barrier);

        if (rc == APTH_BARRIER_SERIAL_THREAD)
        {
            __sync_fetch_and_add(&serial_thread_count, 1);
        }
        else if (rc != 0)
        {
            write(2, "ERROR: barrier_wait returned error\n", 36);
            exit(EXIT_FAILURE);
        }
    }

    return NULL;
}

// Test 2: Barrier with different thread counts
static apth_barrier_t sized_barrier;
static volatile int sized_barrier_passed = 0;

static void* sized_barrier_func(void *arg)
{
    (void)arg;

    int rc = apth_barrier_wait(&sized_barrier);
    if (rc != 0 && rc != APTH_BARRIER_SERIAL_THREAD)
    {
        write(2, "ERROR: sized barrier_wait failed\n", 34);
        exit(EXIT_FAILURE);
    }

    __sync_fetch_and_add(&sized_barrier_passed, 1);

    return NULL;
}

// Test 3: Barrier with work between rounds
static apth_barrier_t work_barrier;
static volatile int work_phase = 0;
static volatile int work_results[N_THREADS][3] = {{0}};

static void* work_barrier_func(void *arg)
{
    int id = (int)(intptr_t)arg;

    // Phase 1: Initialize
    work_results[id][0] = id * 10;
    apth_barrier_wait(&work_barrier);

    // Phase 2: Use results from phase 1
    int sum = 0;
    for (int i = 0; i < N_THREADS; i++)
    {
        sum += work_results[i][0];
    }
    work_results[id][1] = sum;
    apth_barrier_wait(&work_barrier);

    // Phase 3: Verify all threads computed same sum
    for (int i = 0; i < N_THREADS; i++)
    {
        if (work_results[i][1] != work_results[0][1])
        {
            write(2, "ERROR: work results mismatch\n", 30);
            exit(EXIT_FAILURE);
        }
    }
    work_results[id][2] = 1; // Mark as verified
    apth_barrier_wait(&work_barrier);

    return NULL;
}

// Test 4: Barrier reuse
static apth_barrier_t reuse_barrier;
static volatile int reuse_count = 0;

static void* reuse_barrier_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < 10; i++)
    {
        __sync_fetch_and_add(&reuse_count, 1);
        apth_barrier_wait(&reuse_barrier);
    }

    return NULL;
}

// Test 5: Serial thread identification
static apth_barrier_t serial_barrier;
static volatile int serial_ids[N_ROUNDS] = {-1, -1, -1, -1, -1};

static void* serial_barrier_func(void *arg)
{
    int id = (int)(intptr_t)arg;

    for (int round = 0; round < N_ROUNDS; round++)
    {
        int rc = apth_barrier_wait(&serial_barrier);

        if (rc == APTH_BARRIER_SERIAL_THREAD)
        {
            serial_ids[round] = id;
        }
    }

    return NULL;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    write(2, "test_barrier_advanced: starting\n", 32);

    // Test 1: Basic barrier with multiple rounds
    write(2, "  Test 1: Multiple rounds...\n", 30);
    apth_barrier_init(&basic_barrier, NULL, N_THREADS);

    apth_t basic_threads[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
    {
        apth_create(&basic_threads[i], NULL, basic_barrier_func, (void*)(intptr_t)i);
    }

    for (int i = 0; i < N_THREADS; i++)
    {
        apth_join(basic_threads[i], NULL);
    }

    // Verify all rounds completed
    for (int round = 0; round < N_ROUNDS; round++)
    {
        if (round_counters[round] != N_THREADS)
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                    "ERROR: round %d counter=%d, expected %d\n",
                    round, round_counters[round], N_THREADS);
            write(2, buf, strlen(buf));
            exit(EXIT_FAILURE);
        }
    }

    // Verify exactly one serial thread per round
    if (serial_thread_count != N_ROUNDS)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: serial thread count=%d, expected %d\n",
                serial_thread_count, N_ROUNDS);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_barrier_destroy(&basic_barrier);
    write(2, "    PASS\n", 9);

    // Test 2: Different thread counts
    write(2, "  Test 2: Different thread counts...\n", 38);

    // Test with 3 threads
    apth_barrier_init(&sized_barrier, NULL, 3);
    sized_barrier_passed = 0;

    apth_t sized_threads_3[3];
    for (int i = 0; i < 3; i++)
    {
        apth_create(&sized_threads_3[i], NULL, sized_barrier_func, NULL);
    }

    for (int i = 0; i < 3; i++)
    {
        apth_join(sized_threads_3[i], NULL);
    }

    if (sized_barrier_passed != 3)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: 3-thread barrier passed=%d, expected 3\n",
                sized_barrier_passed);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_barrier_destroy(&sized_barrier);

    // Test with 10 threads
    apth_barrier_init(&sized_barrier, NULL, 10);
    sized_barrier_passed = 0;

    apth_t sized_threads_10[10];
    for (int i = 0; i < 10; i++)
    {
        apth_create(&sized_threads_10[i], NULL, sized_barrier_func, NULL);
    }

    for (int i = 0; i < 10; i++)
    {
        apth_join(sized_threads_10[i], NULL);
    }

    if (sized_barrier_passed != 10)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: 10-thread barrier passed=%d, expected 10\n",
                sized_barrier_passed);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_barrier_destroy(&sized_barrier);
    write(2, "    PASS\n", 9);

    // Test 3: Work between rounds
    write(2, "  Test 3: Work between rounds...\n", 34);
    apth_barrier_init(&work_barrier, NULL, N_THREADS);

    apth_t work_threads[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
    {
        apth_create(&work_threads[i], NULL, work_barrier_func, (void*)(intptr_t)i);
    }

    for (int i = 0; i < N_THREADS; i++)
    {
        apth_join(work_threads[i], NULL);
    }

    // Verify all threads completed verification
    for (int i = 0; i < N_THREADS; i++)
    {
        if (work_results[i][2] != 1)
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                    "ERROR: thread %d did not complete verification\n", i);
            write(2, buf, strlen(buf));
            exit(EXIT_FAILURE);
        }
    }

    apth_barrier_destroy(&work_barrier);
    write(2, "    PASS\n", 9);

    // Test 4: Barrier reuse
    write(2, "  Test 4: Barrier reuse...\n", 28);
    apth_barrier_init(&reuse_barrier, NULL, 5);

    apth_t reuse_threads[5];
    for (int i = 0; i < 5; i++)
    {
        apth_create(&reuse_threads[i], NULL, reuse_barrier_func, NULL);
    }

    for (int i = 0; i < 5; i++)
    {
        apth_join(reuse_threads[i], NULL);
    }

    // Each thread increments 10 times, 5 threads = 50
    if (reuse_count != 50)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                "ERROR: reuse count=%d, expected 50\n", reuse_count);
        write(2, buf, strlen(buf));
        exit(EXIT_FAILURE);
    }

    apth_barrier_destroy(&reuse_barrier);
    write(2, "    PASS\n", 9);

    // Test 5: Serial thread identification
    write(2, "  Test 5: Serial thread identification...\n", 43);
    apth_barrier_init(&serial_barrier, NULL, N_THREADS);

    apth_t serial_threads[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
    {
        apth_create(&serial_threads[i], NULL, serial_barrier_func, (void*)(intptr_t)i);
    }

    for (int i = 0; i < N_THREADS; i++)
    {
        apth_join(serial_threads[i], NULL);
    }

    // Verify exactly one serial thread per round
    for (int round = 0; round < N_ROUNDS; round++)
    {
        if (serial_ids[round] < 0 || serial_ids[round] >= N_THREADS)
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                    "ERROR: round %d serial_id=%d invalid\n",
                    round, serial_ids[round]);
            write(2, buf, strlen(buf));
            exit(EXIT_FAILURE);
        }
    }

    apth_barrier_destroy(&serial_barrier);
    write(2, "    PASS\n", 9);

    write(2, "test_barrier_advanced: ALL TESTS PASSED\n", 41);
}
APTH_MAIN_END
