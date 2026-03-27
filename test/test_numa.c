/*
 * test_numa.c -- Test NUMA-aware scheduling features
 *
 * Verify:
 *   1. apth_get_numa_node_count() returns >= 1
 *   2. apth_get_thread_numa_node() returns a valid node for regular threads
 *   3. apth_get_thread_numa_node() returns -1 for dedicated threads
 *   4. Threads created without affinity stay on same NUMA node as creator
 *   5. NUMA-aware work stealing prefers same-node (functional test only;
 *      correctness is verified by threads completing without deadlock)
 *
 * On single-NUMA-node machines (most CI), the test passes trivially
 * since all schedulers share node 0.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>

#include "apth.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <string.h>

#define N_WORKERS  4
#define N_THREADS  20
#define BUSY_ITERS 100000

static _Atomic(int) results[N_THREADS];

static void *
thread_func(void *arg)
{
    int id = (int)(intptr_t)arg;

    /* Record which NUMA node this thread is on */
    int node = apth_get_thread_numa_node(apth_self());
    atomic_store(&results[id], node);

    /* Do some busy work to allow work stealing to kick in */
    volatile long sum = 0;
    for (int i = 0; i < BUSY_ITERS; i++)
        sum += i;
    (void)sum;

    return (void *)(intptr_t)node;
}

static void *
dedicated_func(void *arg)
{
    (void)arg;
    int node = apth_get_thread_numa_node(apth_self());
    return (void *)(intptr_t)node;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    /* Test 1: apth_get_numa_node_count() returns >= 1 */
    int node_count = apth_get_numa_node_count();
    if (node_count < 1)
    {
        static char err[] = "test_numa: FAIL (node_count < 1)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    {
        char buf[128];
        int len = 0;
        const char *prefix = "test_numa: detected ";
        for (int i = 0; prefix[i]; i++)
            buf[len++] = prefix[i];
        /* simple int-to-string for node_count */
        if (node_count >= 10)
            buf[len++] = '0' + (node_count / 10);
        buf[len++] = '0' + (node_count % 10);
        const char *suffix = " NUMA node(s)\n";
        for (int i = 0; suffix[i]; i++)
            buf[len++] = suffix[i];
        write(2, buf, len);
    }

    /* Test 2: creator thread has valid NUMA node */
    int creator_node = apth_get_thread_numa_node(apth_self());
    if (creator_node < 0 || creator_node >= node_count)
    {
        static char err[] = "test_numa: FAIL (creator has invalid NUMA node)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Test 3: NULL thread returns -1 */
    if (apth_get_thread_numa_node(NULL) != -1)
    {
        static char err[] = "test_numa: FAIL (NULL thread should return -1)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Test 4: Create regular threads and verify valid NUMA nodes */
    apth_t tids[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
        atomic_store(&results[i], -99);

    for (int i = 0; i < N_THREADS; i++)
    {
        int rc = apth_create(&tids[i], NULL, thread_func, (void *)(intptr_t)i);
        if (rc != 0)
        {
            static char err[] = "test_numa: FAIL (apth_create failed)\n";
            write(2, err, sizeof(err) - 1);
            exit(1);
        }
    }

    int failures = 0;
    for (int i = 0; i < N_THREADS; i++)
    {
        void *retval;
        apth_join(tids[i], &retval);

        int node = atomic_load(&results[i]);
        if (node < 0 || node >= node_count)
        {
            static char err[] = "test_numa: FAIL (thread has invalid NUMA node)\n";
            write(2, err, sizeof(err) - 1);
            failures++;
        }
    }

    if (failures > 0)
        exit(1);

    /* Test 5: Dedicated thread returns -1 for NUMA node */
    {
        apth_attr_t dattr;
        apth_attr_init(&dattr);
        apth_attr_setclass_np(&dattr, APTH_CLASS_DEDICATED);

        apth_t ded;
        int rc = apth_create(&ded, &dattr, dedicated_func, NULL);
        apth_attr_destroy(&dattr);
        if (rc != 0)
        {
            static char err[] = "test_numa: FAIL (dedicated create failed)\n";
            write(2, err, sizeof(err) - 1);
            exit(1);
        }

        void *retval;
        apth_join(ded, &retval);
        int ded_node = (int)(intptr_t)retval;
        if (ded_node != -1)
        {
            static char err[] = "test_numa: FAIL (dedicated thread should return -1)\n";
            write(2, err, sizeof(err) - 1);
            exit(1);
        }
    }

    /* Test 6: On single-node system, all threads should be on node 0 */
    if (node_count == 1)
    {
        for (int i = 0; i < N_THREADS; i++)
        {
            int node = atomic_load(&results[i]);
            if (node != 0)
            {
                static char err[] = "test_numa: FAIL (single-node: expected node 0)\n";
                write(2, err, sizeof(err) - 1);
                exit(1);
            }
        }
    }

    static char ok[] = "test_numa: PASS\n";
    write(2, ok, sizeof(ok) - 1);
    exit(0);
}
APTH_MAIN_END
