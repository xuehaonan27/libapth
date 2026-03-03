#include "apth.h"
#include "utils/apth_string.h"
#include <stdlib.h>

#define BUFFER_SIZE 5
#define N_ITEMS 100
#define N_WORKERS 2

static apth_sem_t empty_slots;
static apth_sem_t full_slots;
static apth_mutex_t buf_mtx;

static int buffer[BUFFER_SIZE];
static int buf_in = 0;
static int buf_out = 0;

static void *
producer_func(void *arg)
{
    (void)arg;

    for (int i = 1; i <= N_ITEMS; i++)
    {
        apth_sem_wait(&empty_slots);
        apth_mutex_lock(&buf_mtx);

        buffer[buf_in] = i;
        buf_in = (buf_in + 1) % BUFFER_SIZE;

        apth_mutex_unlock(&buf_mtx);
        apth_sem_post(&full_slots);
    }

    return NULL;
}

static volatile long consumer_sum = 0;

static void *
consumer_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < N_ITEMS; i++)
    {
        apth_sem_wait(&full_slots);
        apth_mutex_lock(&buf_mtx);

        int item = buffer[buf_out];
        buf_out = (buf_out + 1) % BUFFER_SIZE;

        apth_mutex_unlock(&buf_mtx);
        apth_sem_post(&empty_slots);

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

    static char msg[] = "test_sem: starting\n";
    write(2, msg, sizeof(msg) - 1);

    apth_sem_init(&empty_slots, 0, BUFFER_SIZE);
    apth_sem_init(&full_slots, 0, 0);
    apth_mutex_init(&buf_mtx, NULL);

    // Test trywait on empty semaphore
    int rc = apth_sem_trywait(&full_slots);
    if (rc != EAGAIN)
    {
        static char err[] = "test_sem: trywait should return EAGAIN\n";
        write(2, err, sizeof(err) - 1);
        exit(EXIT_FAILURE);
    }

    // Test getvalue
    int val;
    apth_sem_getvalue(&empty_slots, &val);
    if (val != BUFFER_SIZE)
    {
        static char err[] = "test_sem: getvalue mismatch\n";
        write(2, err, sizeof(err) - 1);
        exit(EXIT_FAILURE);
    }

    apth_t producer, consumer;
    apth_attr_t attr;

    apth_attr_init(&attr);
    apth_attr_setstacksize(&attr, 4096);

    apth_create(&producer, &attr, producer_func, NULL);
    apth_create(&consumer, &attr, consumer_func, NULL);
    apth_attr_destroy(&attr);

    apth_join(producer, NULL);
    apth_join(consumer, NULL);

    long expected = (long)N_ITEMS * (N_ITEMS + 1) / 2;
    if (consumer_sum != expected)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_sem: FAIL sum=%ld expected=%ld\n",
                                consumer_sum, expected);
        write(2, buf, len);
        exit(EXIT_FAILURE);
    }

    apth_sem_destroy(&empty_slots);
    apth_sem_destroy(&full_slots);
    apth_mutex_destroy(&buf_mtx);

    static char ok[] = "test_sem: PASS\n";
    write(2, ok, sizeof(ok) - 1);
}
APTH_MAIN_END
