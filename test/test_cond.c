#include "apth.h"
#include "utils/apth_string.h"
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 10
#define N_ITEMS 200
#define N_PRODUCERS 2
#define N_CONSUMERS 2
#define N_WORKERS 2

static apth_mutex_t mtx;
static apth_cond_t not_full;
static apth_cond_t not_empty;

static int buffer[BUFFER_SIZE];
static int buf_count = 0;
static int buf_in = 0;
static int buf_out = 0;

static volatile int items_produced = 0;
static volatile int items_consumed = 0;
static volatile long consumer_sum = 0;

static void *
producer_func(void *arg)
{
    int id = (int)(intptr_t)arg;
    int items_per_producer = N_ITEMS / N_PRODUCERS;

    for (int i = 0; i < items_per_producer; i++)
    {
        int item = id * items_per_producer + i + 1;

        apth_mutex_lock(&mtx);
        while (buf_count == BUFFER_SIZE)
            apth_cond_wait(&not_full, &mtx);

        buffer[buf_in] = item;
        buf_in = (buf_in + 1) % BUFFER_SIZE;
        buf_count++;
        items_produced++;

        apth_cond_signal(&not_empty);
        apth_mutex_unlock(&mtx);
    }

    return NULL;
}

static void *
consumer_func(void *arg)
{
    (void)arg;
    int total = N_ITEMS / N_CONSUMERS;

    for (int i = 0; i < total; i++)
    {
        apth_mutex_lock(&mtx);
        while (buf_count == 0)
            apth_cond_wait(&not_empty, &mtx);

        int item = buffer[buf_out];
        buf_out = (buf_out + 1) % BUFFER_SIZE;
        buf_count--;
        items_consumed++;
        consumer_sum += item;

        apth_cond_signal(&not_full);
        apth_mutex_unlock(&mtx);
    }

    return NULL;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char msg[] = "test_cond: starting\n";
    write(2, msg, sizeof(msg) - 1);

    apth_mutex_init(&mtx, NULL);
    apth_cond_init(&not_full, NULL);
    apth_cond_init(&not_empty, NULL);

    apth_t producers[N_PRODUCERS];
    apth_t consumers[N_CONSUMERS];

    for (int i = 0; i < N_CONSUMERS; i++)
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        apth_create(&consumers[i], &attr, consumer_func, (void *)(intptr_t)i);
        apth_attr_destroy(&attr);
    }

    for (int i = 0; i < N_PRODUCERS; i++)
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        apth_create(&producers[i], &attr, producer_func, (void *)(intptr_t)i);
        apth_attr_destroy(&attr);
    }

    for (int i = 0; i < N_PRODUCERS; i++)
        apth_join(producers[i], NULL);
    for (int i = 0; i < N_CONSUMERS; i++)
        apth_join(consumers[i], NULL);

    int failures = 0;

    if (items_produced != N_ITEMS)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_cond: produced=%d expected=%d\n",
                                items_produced, N_ITEMS);
        write(2, buf, len);
        failures++;
    }

    if (items_consumed != N_ITEMS)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_cond: consumed=%d expected=%d\n",
                                items_consumed, N_ITEMS);
        write(2, buf, len);
        failures++;
    }

    // Check that the sum of all consumed items matches expected.
    // Items are 1..N_ITEMS (non-contiguous per-producer but sum is deterministic).
    long expected_sum = 0;
    for (int i = 1; i <= N_ITEMS; i++)
        expected_sum += i;

    if (consumer_sum != expected_sum)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_cond: sum=%ld expected=%ld\n",
                                consumer_sum, expected_sum);
        write(2, buf, len);
        failures++;
    }

    apth_cond_destroy(&not_empty);
    apth_cond_destroy(&not_full);
    apth_mutex_destroy(&mtx);

    if (failures == 0)
    {
        static char ok[] = "test_cond: PASS\n";
        write(2, ok, sizeof(ok) - 1);
    }
    else
    {
        static char fail[] = "test_cond: FAIL\n";
        write(2, fail, sizeof(fail) - 1);
        exit(EXIT_FAILURE);
    }
}
APTH_MAIN_END
