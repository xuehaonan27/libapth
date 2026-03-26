#include "apth.h"
#include "utils/apth_string.h"
#include <stdlib.h>
#include <stdint.h>

/*
 * Test: condition variable between dedicated and regular apth threads.
 *
 * Phase 1 (dedicated produces, regular consumes):
 *   1 dedicated thread produces 100 items into a bounded queue.
 *   2 regular apths consume from the queue.
 *   Verify total consumed == 100.
 *
 * Phase 2 (regular produces, dedicated consumes):
 *   1 regular apth produces 100 items.
 *   1 dedicated thread consumes all 100.
 *   Verify total consumed == 100.
 */

#define QUEUE_SIZE   10
#define N_ITEMS      100
#define N_CONSUMERS  2
#define N_WORKERS    2

/* ---------- shared queue ---------- */

static apth_mutex_t mtx;
static apth_cond_t  not_full;
static apth_cond_t  not_empty;

static int queue[QUEUE_SIZE];
static int q_head  = 0;
static int q_tail  = 0;
static int q_count = 0;

static volatile int total_consumed = 0;

static void
queue_reset(void)
{
    q_head = 0;
    q_tail = 0;
    q_count = 0;
    total_consumed = 0;
}

/* ---------- Phase 1: dedicated producer, regular consumers ---------- */

static void *
phase1_producer(void *arg)
{
    (void)arg;

    for (int i = 1; i <= N_ITEMS; i++)
    {
        apth_mutex_lock(&mtx);
        while (q_count == QUEUE_SIZE)
            apth_cond_wait(&not_full, &mtx);

        queue[q_tail] = i;
        q_tail = (q_tail + 1) % QUEUE_SIZE;
        q_count++;

        apth_cond_signal(&not_empty);
        apth_mutex_unlock(&mtx);
    }

    return NULL;
}

static void *
phase1_consumer(void *arg)
{
    int my_count = (int)(intptr_t)arg; /* number of items to consume */

    for (int i = 0; i < my_count; i++)
    {
        apth_mutex_lock(&mtx);
        while (q_count == 0)
            apth_cond_wait(&not_empty, &mtx);

        /* consume */
        (void)queue[q_head];
        q_head = (q_head + 1) % QUEUE_SIZE;
        q_count--;
        total_consumed++;

        apth_cond_signal(&not_full);
        apth_mutex_unlock(&mtx);
    }

    return NULL;
}

/* ---------- Phase 2: regular producer, dedicated consumer ---------- */

static apth_mutex_t mtx2;
static apth_cond_t  not_full2;
static apth_cond_t  not_empty2;

static int queue2[QUEUE_SIZE];
static int q2_head  = 0;
static int q2_tail  = 0;
static int q2_count = 0;

static volatile int total_consumed2 = 0;

static void *
phase2_producer(void *arg)
{
    (void)arg;

    for (int i = 1; i <= N_ITEMS; i++)
    {
        apth_mutex_lock(&mtx2);
        while (q2_count == QUEUE_SIZE)
            apth_cond_wait(&not_full2, &mtx2);

        queue2[q2_tail] = i;
        q2_tail = (q2_tail + 1) % QUEUE_SIZE;
        q2_count++;

        apth_cond_signal(&not_empty2);
        apth_mutex_unlock(&mtx2);
    }

    return NULL;
}

static void *
phase2_consumer(void *arg)
{
    (void)arg;

    for (int i = 0; i < N_ITEMS; i++)
    {
        apth_mutex_lock(&mtx2);
        while (q2_count == 0)
            apth_cond_wait(&not_empty2, &mtx2);

        /* consume */
        (void)queue2[q2_head];
        q2_head = (q2_head + 1) % QUEUE_SIZE;
        q2_count--;
        total_consumed2++;

        apth_cond_signal(&not_full2);
        apth_mutex_unlock(&mtx2);
    }

    return NULL;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    int failures = 0;

    static char msg[] = "test_dedicated_cond: starting\n";
    write(2, msg, sizeof(msg) - 1);

    /* ---- Phase 1 ---- */
    apth_mutex_init(&mtx, NULL);
    apth_cond_init(&not_full, NULL);
    apth_cond_init(&not_empty, NULL);
    queue_reset();

    apth_t producer;
    apth_t consumers[N_CONSUMERS];

    /* Dedicated producer */
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
        apth_create(&producer, &attr, phase1_producer, NULL);
        apth_attr_destroy(&attr);
    }

    /* Regular consumers: split N_ITEMS evenly */
    for (int i = 0; i < N_CONSUMERS; i++)
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        int items = N_ITEMS / N_CONSUMERS;
        apth_create(&consumers[i], &attr, phase1_consumer,
                     (void *)(intptr_t)items);
        apth_attr_destroy(&attr);
    }

    apth_join(producer, NULL);
    for (int i = 0; i < N_CONSUMERS; i++)
        apth_join(consumers[i], NULL);

    if (total_consumed != N_ITEMS)
    {
        char buf[128];
        int len = apth_snprintf(
            buf, sizeof(buf),
            "test_dedicated_cond: phase1 consumed=%d expected=%d\n",
            total_consumed, N_ITEMS);
        write(2, buf, len);
        failures++;
    }

    apth_cond_destroy(&not_empty);
    apth_cond_destroy(&not_full);
    apth_mutex_destroy(&mtx);

    /* ---- Phase 2 ---- */
    apth_mutex_init(&mtx2, NULL);
    apth_cond_init(&not_full2, NULL);
    apth_cond_init(&not_empty2, NULL);

    apth_t producer2;
    apth_t consumer2;

    /* Regular producer */
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        apth_create(&producer2, &attr, phase2_producer, NULL);
        apth_attr_destroy(&attr);
    }

    /* Dedicated consumer */
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
        apth_create(&consumer2, &attr, phase2_consumer, NULL);
        apth_attr_destroy(&attr);
    }

    apth_join(producer2, NULL);
    apth_join(consumer2, NULL);

    if (total_consumed2 != N_ITEMS)
    {
        char buf[128];
        int len = apth_snprintf(
            buf, sizeof(buf),
            "test_dedicated_cond: phase2 consumed=%d expected=%d\n",
            total_consumed2, N_ITEMS);
        write(2, buf, len);
        failures++;
    }

    apth_cond_destroy(&not_empty2);
    apth_cond_destroy(&not_full2);
    apth_mutex_destroy(&mtx2);

    if (failures == 0)
    {
        static char ok[] = "test_dedicated_cond: PASS\n";
        write(2, ok, sizeof(ok) - 1);
        exit(0);
    }
    else
    {
        static char fail[] = "test_dedicated_cond: FAIL\n";
        write(2, fail, sizeof(fail) - 1);
        exit(EXIT_FAILURE);
    }
}
APTH_MAIN_END
