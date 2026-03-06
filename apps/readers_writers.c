/*
 * readers_writers.c - Readers-Writers Problem using LIBAPTH
 *
 * Demonstrates:
 * - Read-write locks (rwlock)
 * - Multiple reader threads accessing shared data concurrently
 * - Writer threads with exclusive access
 * - Thread priorities and fairness
 * - Barrier synchronization for coordinated start
 *
 * Usage: ./readers_writers <num_readers> <num_writers> <iterations>
 *   num_readers  - Number of reader threads (default: 5)
 *   num_writers  - Number of writer threads (default: 2)
 *   iterations   - Operations per thread (default: 10)
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
#define DEFAULT_READERS 5
#define DEFAULT_WRITERS 2
#define DEFAULT_ITERATIONS 10
#define DATA_SIZE 1024

/* Shared data structure */
typedef struct {
    char data[DATA_SIZE];
    int version;
    apth_rwlock_t lock;
    _Atomic unsigned long read_count;
    _Atomic unsigned long write_count;
    _Atomic int active_readers;
    _Atomic int active_writers;
} shared_data_t;

/* Global state */
typedef struct {
    int num_readers;
    int num_writers;
    int iterations;
    shared_data_t shared;
    apth_barrier_t start_barrier;
    apth_mutex_t stats_mutex;
    unsigned long *reader_stats;
    unsigned long *writer_stats;
} app_state_t;

static app_state_t g_state;

/* Initialize shared data */
void shared_data_init(shared_data_t *sd) {
    memset(sd->data, 0, sizeof(sd->data));
    snprintf(sd->data, sizeof(sd->data), "Initial data - version 0");
    sd->version = 0;
    apth_rwlock_init(&sd->lock, NULL);
    atomic_store(&sd->read_count, 0);
    atomic_store(&sd->write_count, 0);
    atomic_store(&sd->active_readers, 0);
    atomic_store(&sd->active_writers, 0);
}

void shared_data_destroy(shared_data_t *sd) {
    apth_rwlock_destroy(&sd->lock);
}

/* Reader thread */
void *reader_thread(void *arg) {
    int reader_id = *(int *)arg;
    unsigned long local_reads = 0;

    printf("[Reader %d] Ready\n", reader_id);
    apth_barrier_wait(&g_state.start_barrier);

    for (int i = 0; i < g_state.iterations; i++) {
        usleep((rand() % 50) * 1000);

        apth_rwlock_rdlock(&g_state.shared.lock);

        atomic_fetch_add(&g_state.shared.active_readers, 1);
        int active_readers = atomic_load(&g_state.shared.active_readers);
        int active_writers = atomic_load(&g_state.shared.active_writers);

        if (active_writers > 0) {
            printf("[Reader %d] ERROR: Writer active while reading!\n", reader_id);
        }

        int version = g_state.shared.version;
        char data_copy[256];
        strncpy(data_copy, g_state.shared.data, sizeof(data_copy) - 1);
        data_copy[sizeof(data_copy) - 1] = '\0';

        usleep((rand() % 30) * 1000);

        printf("[Reader %d] Read version %d (active readers: %d): %.50s...\n",
               reader_id, version, active_readers, data_copy);

        atomic_fetch_sub(&g_state.shared.active_readers, 1);
        atomic_fetch_add(&g_state.shared.read_count, 1);
        local_reads++;

        apth_rwlock_unlock(&g_state.shared.lock);

        apth_yield();
    }

    apth_mutex_lock(&g_state.stats_mutex);
    g_state.reader_stats[reader_id] = local_reads;
    apth_mutex_unlock(&g_state.stats_mutex);

    printf("[Reader %d] Finished - %lu reads\n", reader_id, local_reads);
    return NULL;
}

/* Writer thread */
void *writer_thread(void *arg) {
    int writer_id = *(int *)arg;
    unsigned long local_writes = 0;

    printf("[Writer %d] Ready\n", writer_id);
    apth_barrier_wait(&g_state.start_barrier);

    for (int i = 0; i < g_state.iterations; i++) {
        usleep((rand() % 100) * 1000);

        apth_rwlock_wrlock(&g_state.shared.lock);

        atomic_fetch_add(&g_state.shared.active_writers, 1);
        int active_readers = atomic_load(&g_state.shared.active_readers);
        int active_writers = atomic_load(&g_state.shared.active_writers);

        if (active_readers > 0) {
            printf("[Writer %d] ERROR: Readers active while writing!\n", writer_id);
        }
        if (active_writers > 1) {
            printf("[Writer %d] ERROR: Multiple writers active!\n", writer_id);
        }

        g_state.shared.version++;
        snprintf(g_state.shared.data, sizeof(g_state.shared.data),
                 "Data written by Writer %d - version %d - iteration %d",
                 writer_id, g_state.shared.version, i);

        usleep((rand() % 50) * 1000);

        printf("[Writer %d] Wrote version %d\n", writer_id, g_state.shared.version);

        atomic_fetch_sub(&g_state.shared.active_writers, 1);
        atomic_fetch_add(&g_state.shared.write_count, 1);
        local_writes++;

        apth_rwlock_unlock(&g_state.shared.lock);

        apth_yield();
    }

    apth_mutex_lock(&g_state.stats_mutex);
    g_state.writer_stats[writer_id] = local_writes;
    apth_mutex_unlock(&g_state.stats_mutex);

    printf("[Writer %d] Finished - %lu writes\n", writer_id, local_writes);
    return NULL;
}

/* Monitor thread */
void *monitor_thread(void *arg) {
    (void)arg;
    printf("[Monitor] Started\n");

    int total_threads = g_state.num_readers + g_state.num_writers;
    apth_barrier_wait(&g_state.start_barrier);

    while (atomic_load(&g_state.shared.read_count) + atomic_load(&g_state.shared.write_count) <
           (unsigned long)(total_threads * g_state.iterations)) {
        sleep(1);

        printf("[Monitor] Reads: %lu, Writes: %lu, Version: %d, Active R/W: %d/%d\n",
               atomic_load(&g_state.shared.read_count),
               atomic_load(&g_state.shared.write_count),
               g_state.shared.version,
               atomic_load(&g_state.shared.active_readers),
               atomic_load(&g_state.shared.active_writers));
    }

    printf("[Monitor] Finished\n");
    return NULL;
}

/* Main application entry point */
APTH_CONFIG(cfg, cfg->workers = 4;)

APTH_MAIN_BEGIN(argc, argv)
{
    g_state.num_readers = DEFAULT_READERS;
    g_state.num_writers = DEFAULT_WRITERS;
    g_state.iterations = DEFAULT_ITERATIONS;

    if (argc > 1) g_state.num_readers = atoi(argv[1]);
    if (argc > 2) g_state.num_writers = atoi(argv[2]);
    if (argc > 3) g_state.iterations = atoi(argv[3]);

    printf("===========================================\n");
    printf("LIBAPTH Readers-Writers\n");
    printf("===========================================\n");
    printf("Readers:    %d\n", g_state.num_readers);
    printf("Writers:    %d\n", g_state.num_writers);
    printf("Iterations: %d\n", g_state.iterations);
    printf("===========================================\n\n");

    srand(time(NULL));

    shared_data_init(&g_state.shared);
    apth_barrier_init(&g_state.start_barrier,
                      NULL,
                      g_state.num_readers + g_state.num_writers + 1);
    apth_mutex_init(&g_state.stats_mutex, NULL);

    g_state.reader_stats = calloc(g_state.num_readers, sizeof(unsigned long));
    g_state.writer_stats = calloc(g_state.num_writers, sizeof(unsigned long));

    apth_t readers[g_state.num_readers];
    apth_t writers[g_state.num_writers];
    apth_t monitor;
    int reader_ids[g_state.num_readers];
    int writer_ids[g_state.num_writers];

    struct timespec start_time, end_time;

    for (int i = 0; i < g_state.num_readers; i++) {
        reader_ids[i] = i;
        if (apth_create(&readers[i], NULL, reader_thread, &reader_ids[i]) != 0) {
            fprintf(stderr, "Failed to create reader thread %d\n", i);
            exit(1);
        }
    }

    for (int i = 0; i < g_state.num_writers; i++) {
        writer_ids[i] = i;
        if (apth_create(&writers[i], NULL, writer_thread, &writer_ids[i]) != 0) {
            fprintf(stderr, "Failed to create writer thread %d\n", i);
            exit(1);
        }
    }

    if (apth_create(&monitor, NULL, monitor_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create monitor thread\n");
        exit(1);
    }

    printf("\n[Main] All threads created, starting synchronized execution...\n\n");
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (int i = 0; i < g_state.num_readers; i++) {
        apth_join(readers[i], NULL);
    }

    for (int i = 0; i < g_state.num_writers; i++) {
        apth_join(writers[i], NULL);
    }

    apth_join(monitor, NULL);

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                     (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    printf("\n===========================================\n");
    printf("Results\n");
    printf("===========================================\n");
    printf("Total reads:     %lu\n", atomic_load(&g_state.shared.read_count));
    printf("Total writes:    %lu\n", atomic_load(&g_state.shared.write_count));
    printf("Final version:   %d\n", g_state.shared.version);
    printf("Elapsed time:    %.2f seconds\n", elapsed);
    printf("Operations/sec:  %.2f\n",
           (atomic_load(&g_state.shared.read_count) + atomic_load(&g_state.shared.write_count)) / elapsed);
    printf("\nFinal data: %.80s\n", g_state.shared.data);
    printf("===========================================\n");

    shared_data_destroy(&g_state.shared);
    apth_barrier_destroy(&g_state.start_barrier);
    apth_mutex_destroy(&g_state.stats_mutex);
    free(g_state.reader_stats);
    free(g_state.writer_stats);

    exit(0);
}
APTH_MAIN_END
