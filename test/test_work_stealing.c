#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif // _GNU_SOURCE
#include <sched.h>

#include "apth.h"
#include "utils/apth_string.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define N_CHILDREN 200
#define N_WORKERS 4
#define BUSY_ITERS 1000000

static void *
thread_func(void *arg)
{
    int id = (int)(intptr_t)arg;

    char name_buf[32];
    apth_snprintf(name_buf, sizeof(name_buf), "STEAL_%d", id);
    apth_setname_np(apth_self(), name_buf);

    // CPU-bound busy work: keeps the thread in ready/running state
    // without doing I/O, maximizing the chance other schedulers steal it.
    volatile long sum = 0;
    for (int i = 0; i < BUSY_ITERS; i++)
        sum += i;
    (void)sum;

    return (void *)(intptr_t)id;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

static apth_t tids[N_CHILDREN];
static void *cdatas[N_CHILDREN];

APTH_MAIN_BEGIN(argc, argv)
{
    static char main_hi[] = "test_work_stealing: creating all threads on worker 0\n";
    write(2, main_hi, sizeof(main_hi) - 1);

    // Create ALL threads pinned to CPU 0 via affinity.
    // This forces all N_CHILDREN threads onto scheduler 0.
    // Idle schedulers 1..N_WORKERS-1 should steal from scheduler 0's ready queue.
    for (int i = 0; i < N_CHILDREN; i++)
    {
        apth_attr_t child_attr;
        apth_attr_init(&child_attr);

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        apth_attr_setaffinity_np(&child_attr, sizeof(cpuset), &cpuset);

        apth_attr_setstacksize(&child_attr, 4096);

        int rc = apth_create(&tids[i], &child_attr, thread_func, (void *)(intptr_t)i);
        if (rc != 0)
        {
            char err_buf[128];
            int len = apth_snprintf(err_buf, sizeof(err_buf),
                                    "apth_create failed for thread %d: %s\n",
                                    i, strerror(rc));
            write(2, err_buf, len);
            exit(EXIT_FAILURE);
        }
        apth_attr_destroy(&child_attr);
    }

    static char join_msg[] = "test_work_stealing: joining threads\n";
    write(2, join_msg, sizeof(join_msg) - 1);

    for (int i = 0; i < N_CHILDREN; i++)
    {
        apth_join(tids[i], &cdatas[i]);
    }

    int failures = 0;
    for (int i = 0; i < N_CHILDREN; i++)
    {
        if ((int)(intptr_t)cdatas[i] != i)
        {
            char err_buf[128];
            int len = apth_snprintf(err_buf, sizeof(err_buf),
                                    "FAIL: thread %d returned %d\n",
                                    i, (int)(intptr_t)cdatas[i]);
            write(2, err_buf, len);
            failures++;
        }
    }

    if (failures == 0)
    {
        static char ok_msg[] = "test_work_stealing: PASS\n";
        write(2, ok_msg, sizeof(ok_msg) - 1);
    }
    else
    {
        static char fail_msg[] = "test_work_stealing: FAIL\n";
        write(2, fail_msg, sizeof(fail_msg) - 1);
        exit(EXIT_FAILURE);
    }
}
APTH_MAIN_END
