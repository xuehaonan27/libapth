/*
 * file_processor.c - Concurrent File Processing Application using LIBAPTH
 *
 * A comprehensive test application that exercises:
 * - File I/O operations (read, write)
 * - Thread pool with work queue
 * - Semaphore for rate limiting
 * - Read-write locks for shared data structures
 * - Thread-specific data
 * - Barrier synchronization
 *
 * This application processes multiple text files concurrently, performing
 * various operations like word counting, character frequency analysis, etc.
 *
 * Usage: ./file_processor <input_dir> <output_dir> <num_workers>
 *   input_dir   - Directory containing input files (default: ./input)
 *   output_dir  - Directory for output files (default: ./output)
 *   num_workers - Number of worker threads (default: 4)
 */

#define _GNU_SOURCE
#include "apth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>
#include <stdatomic.h>

/* Configuration */
#define DEFAULT_INPUT_DIR "./input"
#define DEFAULT_OUTPUT_DIR "./output"
#define DEFAULT_WORKERS 4
#define MAX_PATH 1024
#define MAX_FILES 256
#define BUFFER_SIZE 8192
#define MAX_CONCURRENT_FILES 8

/* File processing task */
typedef struct {
    char input_path[MAX_PATH];
    char output_path[MAX_PATH];
    int task_id;
} file_task_t;

/* Work queue */
typedef struct {
    file_task_t tasks[MAX_FILES];
    int head;
    int tail;
    int count;
    apth_mutex_t mutex;
    apth_cond_t not_empty;
} work_queue_t;

/* Statistics per worker */
typedef struct {
    unsigned long files_processed;
    unsigned long bytes_read;
    unsigned long bytes_written;
    unsigned long words_counted;
} worker_stats_t;

/* Global state */
typedef struct {
    char input_dir[MAX_PATH];
    char output_dir[MAX_PATH];
    int num_workers;
    work_queue_t queue;
    apth_sem_t rate_limiter;
    apth_rwlock_t stats_lock;
    _Atomic int shutdown_flag;
    _Atomic unsigned long total_files;
    _Atomic unsigned long completed_files;
    worker_stats_t *worker_stats;
} processor_state_t;

static processor_state_t g_state;
static apth_key_t g_worker_id_key;

/* Queue operations */
void queue_init(work_queue_t *q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    apth_mutex_init(&q->mutex, NULL);
    apth_cond_init(&q->not_empty, NULL);
}

void queue_destroy(work_queue_t *q) {
    apth_mutex_destroy(&q->mutex);
    apth_cond_destroy(&q->not_empty);
}

int queue_push(work_queue_t *q, file_task_t task) {
    apth_mutex_lock(&q->mutex);

    if (q->count >= MAX_FILES) {
        apth_mutex_unlock(&q->mutex);
        return -1;
    }

    q->tasks[q->tail] = task;
    q->tail = (q->tail + 1) % MAX_FILES;
    q->count++;

    apth_cond_signal(&q->not_empty);
    apth_mutex_unlock(&q->mutex);
    return 0;
}

int queue_pop(work_queue_t *q, file_task_t *task) {
    apth_mutex_lock(&q->mutex);

    while (q->count == 0 && !atomic_load(&g_state.shutdown_flag)) {
        apth_cond_wait(&q->not_empty, &q->mutex);
    }

    if (q->count == 0 && atomic_load(&g_state.shutdown_flag)) {
        apth_mutex_unlock(&q->mutex);
        return -1;
    }

    *task = q->tasks[q->head];
    q->head = (q->head + 1) % MAX_FILES;
    q->count--;

    apth_mutex_unlock(&q->mutex);
    return 0;
}

/* Process a single file */
int process_file(const char *input_path, const char *output_path, worker_stats_t *stats) {
    int input_fd = open(input_path, O_RDONLY);
    if (input_fd < 0) {
        fprintf(stderr, "Error opening %s: %s\n", input_path, strerror(errno));
        return -1;
    }

    int output_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output_fd < 0) {
        fprintf(stderr, "Error creating %s: %s\n", output_path, strerror(errno));
        close(input_fd);
        return -1;
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    unsigned long words = 0;
    unsigned long chars = 0;
    unsigned long lines = 0;
    int in_word = 0;

    unsigned long freq[256] = {0};

    while ((bytes_read = read(input_fd, buffer, sizeof(buffer))) > 0) {
        stats->bytes_read += bytes_read;

        for (ssize_t i = 0; i < bytes_read; i++) {
            unsigned char c = buffer[i];
            chars++;
            freq[c]++;

            if (c == '\n') {
                lines++;
            }

            if (isspace(c)) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }

    close(input_fd);

    stats->words_counted += words;

    char report[BUFFER_SIZE * 2];
    int report_len = snprintf(report, sizeof(report),
        "File Analysis Report\n"
        "====================\n"
        "Input:  %s\n"
        "Output: %s\n\n"
        "Statistics:\n"
        "  Lines:      %lu\n"
        "  Words:      %lu\n"
        "  Characters: %lu\n\n"
        "Top 10 Most Frequent Characters:\n",
        input_path, output_path, lines, words, chars);

    typedef struct { unsigned char c; unsigned long count; } char_freq_t;
    char_freq_t top[10] = {{0}};

    for (int i = 0; i < 256; i++) {
        if (freq[i] > 0) {
            for (int j = 0; j < 10; j++) {
                if (freq[i] > top[j].count) {
                    for (int k = 9; k > j; k--) {
                        top[k] = top[k-1];
                    }
                    top[j].c = i;
                    top[j].count = freq[i];
                    break;
                }
            }
        }
    }

    for (int i = 0; i < 10 && top[i].count > 0; i++) {
        char c = top[i].c;
        const char *desc = isprint(c) ? "" : " (non-printable)";
        report_len += snprintf(report + report_len, sizeof(report) - report_len,
            "  %3d. '%c' (0x%02x)%s: %lu times\n",
            i + 1, isprint(c) ? c : '.', c, desc, top[i].count);
    }

    ssize_t written = write(output_fd, report, report_len);
    if (written > 0) {
        stats->bytes_written += written;
    }

    close(output_fd);
    stats->files_processed++;

    return 0;
}

/* Worker thread function */
void *worker_thread(void *arg) {
    int worker_id = *(int *)arg;
    apth_setspecific(g_worker_id_key, arg);

    worker_stats_t *stats = &g_state.worker_stats[worker_id];
    memset(stats, 0, sizeof(worker_stats_t));

    printf("[Worker %d] Started\n", worker_id);

    while (!atomic_load(&g_state.shutdown_flag)) {
        file_task_t task;
        if (queue_pop(&g_state.queue, &task) < 0) {
            break;
        }

        apth_sem_wait(&g_state.rate_limiter);

        printf("[Worker %d] Processing: %s\n", worker_id, task.input_path);

        if (process_file(task.input_path, task.output_path, stats) == 0) {
            atomic_fetch_add(&g_state.completed_files, 1);
        }

        apth_sem_post(&g_state.rate_limiter);
    }

    printf("[Worker %d] Finished - Files: %lu, Words: %lu\n",
           worker_id, stats->files_processed, stats->words_counted);

    return NULL;
}

/* Scan directory and enqueue files */
int scan_directory(const char *input_dir, const char *output_dir) {
    DIR *dir = opendir(input_dir);
    if (!dir) {
        fprintf(stderr, "Error opening directory %s: %s\n", input_dir, strerror(errno));
        return -1;
    }

    struct dirent *entry;
    int count = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) {
            continue;
        }

        if (strncmp(entry->d_name, ".", 1) == 0) {
            continue;
        }

        file_task_t task;
        snprintf(task.input_path, sizeof(task.input_path), "%s/%s", input_dir, entry->d_name);
        snprintf(task.output_path, sizeof(task.output_path), "%s/%s.report", output_dir, entry->d_name);
        task.task_id = count;

        if (queue_push(&g_state.queue, task) == 0) {
            count++;
            atomic_fetch_add(&g_state.total_files, 1);
        }
    }

    closedir(dir);
    return count;
}

/* Main application entry point */
APTH_CONFIG(cfg, cfg->workers = 4;)

APTH_MAIN_BEGIN(argc, argv)
{
    strncpy(g_state.input_dir, DEFAULT_INPUT_DIR, sizeof(g_state.input_dir) - 1);
    strncpy(g_state.output_dir, DEFAULT_OUTPUT_DIR, sizeof(g_state.output_dir) - 1);
    g_state.num_workers = DEFAULT_WORKERS;

    if (argc > 1) strncpy(g_state.input_dir, argv[1], sizeof(g_state.input_dir) - 1);
    if (argc > 2) strncpy(g_state.output_dir, argv[2], sizeof(g_state.output_dir) - 1);
    if (argc > 3) g_state.num_workers = atoi(argv[3]);

    printf("===========================================\n");
    printf("LIBAPTH File Processor\n");
    printf("===========================================\n");
    printf("Input Dir:  %s\n", g_state.input_dir);
    printf("Output Dir: %s\n", g_state.output_dir);
    printf("Workers:    %d\n", g_state.num_workers);
    printf("===========================================\n\n");

    mkdir(g_state.output_dir, 0755);

    atomic_store(&g_state.shutdown_flag, 0);
    atomic_store(&g_state.total_files, 0);
    atomic_store(&g_state.completed_files, 0);

    queue_init(&g_state.queue);
    apth_sem_init(&g_state.rate_limiter, 0, MAX_CONCURRENT_FILES);
    apth_rwlock_init(&g_state.stats_lock, NULL);

    g_state.worker_stats = calloc(g_state.num_workers, sizeof(worker_stats_t));
    if (!g_state.worker_stats) {
        fprintf(stderr, "Failed to allocate worker stats\n");
        exit(1);
    }

    apth_key_create(&g_worker_id_key, NULL);

    printf("[Scanner] Scanning input directory...\n");
    int file_count = scan_directory(g_state.input_dir, g_state.output_dir);
    if (file_count < 0) {
        fprintf(stderr, "Failed to scan directory\n");
        exit(1);
    }

    printf("[Scanner] Found %d files to process\n\n", file_count);

    if (file_count == 0) {
        printf("No files to process. Exiting.\n");
        exit(0);
    }

    apth_t workers[g_state.num_workers];
    int worker_ids[g_state.num_workers];

    for (int i = 0; i < g_state.num_workers; i++) {
        worker_ids[i] = i;
        if (apth_create(&workers[i], NULL, worker_thread, &worker_ids[i]) != 0) {
            fprintf(stderr, "Failed to create worker thread %d\n", i);
            exit(1);
        }
    }

    while (atomic_load(&g_state.completed_files) < atomic_load(&g_state.total_files)) {
        sleep(1);
        printf("[Progress] %lu / %lu files completed\n",
               atomic_load(&g_state.completed_files),
               atomic_load(&g_state.total_files));
    }

    atomic_store(&g_state.shutdown_flag, 1);
    apth_cond_broadcast(&g_state.queue.not_empty);

    for (int i = 0; i < g_state.num_workers; i++) {
        apth_join(workers[i], NULL);
    }

    printf("\n===========================================\n");
    printf("Processing Complete\n");
    printf("===========================================\n");

    unsigned long total_files = 0;
    unsigned long total_bytes_read = 0;
    unsigned long total_bytes_written = 0;
    unsigned long total_words = 0;

    for (int i = 0; i < g_state.num_workers; i++) {
        total_files += g_state.worker_stats[i].files_processed;
        total_bytes_read += g_state.worker_stats[i].bytes_read;
        total_bytes_written += g_state.worker_stats[i].bytes_written;
        total_words += g_state.worker_stats[i].words_counted;
    }

    printf("Total files processed: %lu\n", total_files);
    printf("Total bytes read:      %lu (%.2f MB)\n",
           total_bytes_read, total_bytes_read / (1024.0 * 1024.0));
    printf("Total bytes written:   %lu (%.2f MB)\n",
           total_bytes_written, total_bytes_written / (1024.0 * 1024.0));
    printf("Total words counted:   %lu\n", total_words);
    printf("===========================================\n");

    queue_destroy(&g_state.queue);
    apth_sem_destroy(&g_state.rate_limiter);
    apth_rwlock_destroy(&g_state.stats_lock);
    apth_key_delete(g_worker_id_key);
    free(g_state.worker_stats);

    exit(0);
}
APTH_MAIN_END

