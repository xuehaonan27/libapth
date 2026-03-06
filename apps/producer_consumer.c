/*
 * producer_consumer.c - Classic Producer-Consumer Problem using LIBAPTH
 *
 * Demonstrates:
 * - Multiple producer and consumer threads
 * - Mutex and condition variables for synchronization
 * - Bounded buffer implementation
 * - Thread cancellation
 * - Thread-specific data
 * - Once-only initialization
 *
 * Usage: ./producer_consumer <num_producers> <num_consumers> <buffer_size> <items_per_producer>
 *   num_producers       - Number of producer threads (default: 3)
 *   num_consumers       - Number of consumer threads (default: 2)
 *   buffer_size         - Size of shared buffer (default: 10)
 *   items_per_producer  - Items each producer creates (default: 20)
 */

#define _GNU_SOURCE
#include "apth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>

/* Configuration */
#define DEFAULT_PRODUCERS 3
#define DEFAULT_CONSUMERS 2
#define DEFAULT_BUFFER_SIZE 10
#define DEFAULT_ITEMS_PER_PRODUCER 20
#define MAX_BUFFER_SIZE 1024

/* Item in the buffer */
typedef struct {
    int producer_id;
    int sequence_number;
    struct timespec timestamp;
} item_t;

/* Bounded buffer */
typedef struct {
    item_t *items;
    int capacity;
    int count;
    int head;
    int tail;
    apth_mutex_t mutex;
    apth_cond_t not_empty;
    apth_cond_t not_full;
    _Atomic unsigned long total_produced;
    _Atomic unsigned long total_consumed;
} buffer_t;

/* Global state */
typedef struct {
    int num_producers;
    int num_consumers;
    int items_per_producer;
    buffer_t buffer;
    _Atomic int producers_done;
    apth_once_t init_once;
} app_state_t;

static app_state_t g_state;
static apth_key_t g_thread_id_key;

/* Once-only initialization */
void init_random(void) {
    srand(time(NULL));
    printf("[Init] Random number generator initialized (once-only)\n");
}

/* Buffer operations */
void buffer_init(buffer_t *buf, int capacity) {
    buf->items = malloc(capacity * sizeof(item_t));
    buf->capacity = capacity;
    buf->count = 0;
    buf->head = 0;
    buf->tail = 0;
    apth_mutex_init(&buf->mutex, NULL);
    apth_cond_init(&buf->not_empty, NULL);
    apth_cond_init(&buf->not_full, NULL);
    atomic_store(&buf->total_produced, 0);
    atomic_store(&buf->total_consumed, 0);
}

void buffer_destroy(buffer_t *buf) {
    free(buf->items);
    apth_mutex_destroy(&buf->mutex);
    apth_cond_destroy(&buf->not_empty);
    apth_cond_destroy(&buf->not_full);
}

void buffer_produce(buffer_t *buf, item_t item) {
    apth_mutex_lock(&buf->mutex);

    while (buf->count >= buf->capacity) {
        apth_cond_wait(&buf->not_full, &buf->mutex);
    }

    buf->items[buf->tail] = item;
    buf->tail = (buf->tail + 1) % buf->capacity;
    buf->count++;
    atomic_fetch_add(&buf->total_produced, 1);

    apth_cond_signal(&buf->not_empty);
    apth_mutex_unlock(&buf->mutex);
}

int buffer_consume(buffer_t *buf, item_t *item) {
    apth_mutex_lock(&buf->mutex);

    while (buf->count == 0) {
        if (atomic_load(&g_state.producers_done) >= g_state.num_producers) {
            apth_mutex_unlock(&buf->mutex);
            return -1;
        }
        apth_cond_wait(&buf->not_empty, &buf->mutex);
    }

    if (buf->count == 0) {
        apth_mutex_unlock(&buf->mutex);
        return -1;
    }

    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % buf->capacity;
    buf->count--;
    atomic_fetch_add(&buf->total_consumed, 1);

    apth_cond_signal(&buf->not_full);
    apth_mutex_unlock(&buf->mutex);
    return 0;
}

/* Producer thread */
void *producer_thread(void *arg) {
    int producer_id = *(int *)arg;
    apth_setspecific(g_thread_id_key, arg);

    apth_once(&g_state.init_once, init_random);

    printf("[Producer %d] Started\n", producer_id);

    for (int i = 0; i < g_state.items_per_producer; i++) {
        item_t item;
        item.producer_id = producer_id;
        item.sequence_number = i;
        clock_gettime(CLOCK_MONOTONIC, &item.timestamp);

        usleep((rand() % 100) * 1000);

        buffer_produce(&g_state.buffer, item);
        printf("[Producer %d] Produced item #%d\n", producer_id, i);

        apth_testcancel();
    }

    atomic_fetch_add(&g_state.producers_done, 1);
    apth_cond_broadcast(&g_state.buffer.not_empty);

    printf("[Producer %d] Finished - produced %d items\n",
           producer_id, g_state.items_per_producer);

    return NULL;
}

/* Consumer thread */
void *consumer_thread(void *arg) {
    int consumer_id = *(int *)arg;
    apth_setspecific(g_thread_id_key, arg);

    apth_once(&g_state.init_once, init_random);

    printf("[Consumer %d] Started\n", consumer_id);

    int consumed_count = 0;
    while (1) {
        item_t item;
        if (buffer_consume(&g_state.buffer, &item) < 0) {
            break;
        }

        usleep((rand() % 150) * 1000);

        consumed_count++;
        printf("[Consumer %d] Consumed item #%d from Producer %d\n",
               consumer_id, item.sequence_number, item.producer_id);

        apth_testcancel();
    }

    printf("[Consumer %d] Finished - consumed %d items\n",
           consumer_id, consumed_count);

    return NULL;
}

/* Monitor thread - displays statistics */
void *monitor_thread(void *arg) {
    (void)arg;
    printf("[Monitor] Started\n");

    while (atomic_load(&g_state.producers_done) < g_state.num_producers ||
           atomic_load(&g_state.buffer.total_consumed) < atomic_load(&g_state.buffer.total_produced)) {
        sleep(2);

        apth_mutex_lock(&g_state.buffer.mutex);
        int buffer_count = g_state.buffer.count;
        apth_mutex_unlock(&g_state.buffer.mutex);

        printf("[Monitor] Produced: %lu, Consumed: %lu, Buffer: %d/%d, Producers done: %d/%d\n",
               atomic_load(&g_state.buffer.total_produced),
               atomic_load(&g_state.buffer.total_consumed),
               buffer_count,
               g_state.buffer.capacity,
               atomic_load(&g_state.producers_done),
               g_state.num_producers);
    }

    printf("[Monitor] Finished\n");
    return NULL;
}

/* Main application entry point */
APTH_CONFIG(cfg, cfg->workers = 4;)

APTH_MAIN_BEGIN(argc, argv)
{
    g_state.num_producers = DEFAULT_PRODUCERS;
    g_state.num_consumers = DEFAULT_CONSUMERS;
    int buffer_size = DEFAULT_BUFFER_SIZE;
    g_state.items_per_producer = DEFAULT_ITEMS_PER_PRODUCER;

    if (argc > 1) g_state.num_producers = atoi(argv[1]);
    if (argc > 2) g_state.num_consumers = atoi(argv[2]);
    if (argc > 3) buffer_size = atoi(argv[3]);
    if (argc > 4) g_state.items_per_producer = atoi(argv[4]);

    if (buffer_size > MAX_BUFFER_SIZE) buffer_size = MAX_BUFFER_SIZE;

    printf("===========================================\n");
    printf("LIBAPTH Producer-Consumer\n");
    printf("===========================================\n");
    printf("Producers:          %d\n", g_state.num_producers);
    printf("Consumers:          %d\n", g_state.num_consumers);
    printf("Buffer size:        %d\n", buffer_size);
    printf("Items per producer: %d\n", g_state.items_per_producer);
    printf("Total items:        %d\n", g_state.num_producers * g_state.items_per_producer);
    printf("===========================================\n\n");

    atomic_store(&g_state.producers_done, 0);
    g_state.init_once = 0;

    buffer_init(&g_state.buffer, buffer_size);
    apth_key_create(&g_thread_id_key, NULL);

    apth_t producers[g_state.num_producers];
    apth_t consumers[g_state.num_consumers];
    apth_t monitor;
    int producer_ids[g_state.num_producers];
    int consumer_ids[g_state.num_consumers];

    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (int i = 0; i < g_state.num_producers; i++) {
        producer_ids[i] = i;
        if (apth_create(&producers[i], NULL, producer_thread, &producer_ids[i]) != 0) {
            fprintf(stderr, "Failed to create producer thread %d\n", i);
            exit(1);
        }
    }

    for (int i = 0; i < g_state.num_consumers; i++) {
        consumer_ids[i] = i;
        if (apth_create(&consumers[i], NULL, consumer_thread, &consumer_ids[i]) != 0) {
            fprintf(stderr, "Failed to create consumer thread %d\n", i);
            exit(1);
        }
    }

    if (apth_create(&monitor, NULL, monitor_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create monitor thread\n");
        exit(1);
    }

    for (int i = 0; i < g_state.num_producers; i++) {
        apth_join(producers[i], NULL);
    }

    for (int i = 0; i < g_state.num_consumers; i++) {
        apth_join(consumers[i], NULL);
    }

    apth_join(monitor, NULL);

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                     (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    printf("\n===========================================\n");
    printf("Results\n");
    printf("===========================================\n");
    printf("Total produced:  %lu\n", atomic_load(&g_state.buffer.total_produced));
    printf("Total consumed:  %lu\n", atomic_load(&g_state.buffer.total_consumed));
    printf("Elapsed time:    %.2f seconds\n", elapsed);
    printf("Throughput:      %.2f items/sec\n",
           atomic_load(&g_state.buffer.total_consumed) / elapsed);
    printf("===========================================\n");

    buffer_destroy(&g_state.buffer);
    apth_key_delete(g_thread_id_key);

    exit(0);
}
APTH_MAIN_END
