/*
 * signal_demo.c - Signal Handling and Thread Cancellation Demo using LIBAPTH
 *
 * Demonstrates:
 * - Signal handling (SIGUSR1, SIGUSR2, SIGINT, SIGTERM)
 * - Thread cancellation (apth_cancel)
 * - Cleanup handlers (apth_cleanup_push/pop)
 * - Signal masks per thread (apth_sigmask)
 * - Thread-directed signals (apth_kill)
 * - Signal waiting (sigwait)
 *
 * Usage: ./signal_demo <num_workers> <duration>
 *   num_workers - Number of worker threads (default: 4)
 *   duration    - Run duration in seconds (default: 30)
 */

#define _GNU_SOURCE
#include "apth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <stdarg.h>

/* Configuration */
#define DEFAULT_WORKERS 4
#define DEFAULT_DURATION 30

/* Worker state */
typedef struct {
    int id;
    _Atomic unsigned long work_done;
    _Atomic int cancelled;
    _Atomic int sigusr1_count;
    _Atomic int sigusr2_count;
    apth_t thread;
} worker_t;

/* Global state */
typedef struct {
    int num_workers;
    int duration;
    worker_t *workers;
    _Atomic int shutdown_flag;
    _Atomic unsigned long total_work;
    apth_mutex_t print_mutex;
} app_state_t;

static app_state_t g_state;

/* Thread-safe printing */
void safe_print(const char *format, ...) {
    va_list args;
    va_start(args, format);
    apth_mutex_lock(&g_state.print_mutex);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    printf("[%ld.%03ld] ", ts.tv_sec % 1000, ts.tv_nsec / 1000000);
    vprintf(format, args);
    fflush(stdout);
    apth_mutex_unlock(&g_state.print_mutex);
    va_end(args);
}

/* Cleanup handler */
void worker_cleanup(void *arg) {
    worker_t *worker = (worker_t *)arg;
    safe_print("[Worker %d] Cleanup handler called\n", worker->id);
    atomic_store(&worker->cancelled, 1);
}

/* Signal handler for SIGUSR1 */
void sigusr1_handler(int signo) {
    (void)signo;
}

/* Signal handler for SIGUSR2 */
void sigusr2_handler(int signo) {
    (void)signo;
}

/* Worker thread */
void *worker_thread(void *arg) {
    worker_t *worker = (worker_t *)arg;

    apth_cleanup_push(worker_cleanup, worker);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    apth_sigmask(SIG_UNBLOCK, &mask, NULL);

    signal(SIGUSR1, sigusr1_handler);
    signal(SIGUSR2, sigusr2_handler);

    safe_print("[Worker %d] Started\n", worker->id);

    while (!atomic_load(&g_state.shutdown_flag)) {
        usleep((rand() % 100 + 50) * 1000);

        unsigned long work = rand() % 100;
        atomic_fetch_add(&worker->work_done, work);
        atomic_fetch_add(&g_state.total_work, work);

        if (worker->work_done % 500 == 0) {
            safe_print("[Worker %d] Work done: %lu (SIGUSR1: %d, SIGUSR2: %d)\n",
                       worker->id,
                       atomic_load(&worker->work_done),
                       atomic_load(&worker->sigusr1_count),
                       atomic_load(&worker->sigusr2_count));
        }

        apth_testcancel();
    }

    safe_print("[Worker %d] Exiting normally\n", worker->id);

    apth_cleanup_pop(1);
    return NULL;
}

/* Signal monitor thread */
void *signal_monitor_thread(void *arg) {
    (void)arg;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    apth_sigmask(SIG_BLOCK, &mask, NULL);

    safe_print("[Signal Monitor] Started\n");

    while (!atomic_load(&g_state.shutdown_flag)) {
        int sig;
        struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};

        if (sigwait(&mask, &sig) == 0) {
            if (sig == SIGUSR1) {
                safe_print("[Signal Monitor] Received SIGUSR1\n");
            } else if (sig == SIGUSR2) {
                safe_print("[Signal Monitor] Received SIGUSR2\n");
            }
        }
    }

    safe_print("[Signal Monitor] Exiting\n");
    return NULL;
}

/* Controller thread - sends signals and cancels threads */
void *controller_thread(void *arg) {
    (void)arg;

    safe_print("[Controller] Started\n");

    int elapsed = 0;
    while (elapsed < g_state.duration && !atomic_load(&g_state.shutdown_flag)) {
        sleep(3);
        elapsed += 3;

        int action = rand() % 4;
        int target = rand() % g_state.num_workers;

        switch (action) {
        case 0:
            safe_print("[Controller] Sending SIGUSR1 to Worker %d\n", target);
            apth_kill(g_state.workers[target].thread, SIGUSR1);
            atomic_fetch_add(&g_state.workers[target].sigusr1_count, 1);
            break;

        case 1:
            safe_print("[Controller] Sending SIGUSR2 to Worker %d\n", target);
            apth_kill(g_state.workers[target].thread, SIGUSR2);
            atomic_fetch_add(&g_state.workers[target].sigusr2_count, 1);
            break;

        case 2:
            if (elapsed > g_state.duration / 2 && !atomic_load(&g_state.workers[target].cancelled)) {
                safe_print("[Controller] Cancelling Worker %d\n", target);
                apth_cancel(g_state.workers[target].thread);
            }
            break;

        case 3:
            safe_print("[Controller] Status check - Total work: %lu\n",
                       atomic_load(&g_state.total_work));
            break;
        }
    }

    safe_print("[Controller] Shutting down\n");
    atomic_store(&g_state.shutdown_flag, 1);

    return NULL;
}

/* Statistics thread */
void *stats_thread(void *arg) {
    (void)arg;

    safe_print("[Stats] Started\n");

    while (!atomic_load(&g_state.shutdown_flag)) {
        sleep(5);

        safe_print("[Stats] Total work: %lu, Active workers: ",
                   atomic_load(&g_state.total_work));

        int active = 0;
        for (int i = 0; i < g_state.num_workers; i++) {
            if (!atomic_load(&g_state.workers[i].cancelled)) {
                active++;
            }
        }
        printf("%d/%d\n", active, g_state.num_workers);
    }

    safe_print("[Stats] Exiting\n");
    return NULL;
}

/* Global signal handler for SIGINT/SIGTERM */
void shutdown_handler(int signo) {
    (void)signo;
    printf("\n[Main] Received shutdown signal\n");
    atomic_store(&g_state.shutdown_flag, 1);
}

/* Main application entry point */
APTH_CONFIG(cfg, cfg->workers = 4;)

APTH_MAIN_BEGIN(argc, argv)
{
    g_state.num_workers = DEFAULT_WORKERS;
    g_state.duration = DEFAULT_DURATION;

    if (argc > 1) g_state.num_workers = atoi(argv[1]);
    if (argc > 2) g_state.duration = atoi(argv[2]);

    printf("===========================================\n");
    printf("LIBAPTH Signal Handling Demo\n");
    printf("===========================================\n");
    printf("Workers:  %d\n", g_state.num_workers);
    printf("Duration: %d seconds\n", g_state.duration);
    printf("\nThis demo will:\n");
    printf("- Send SIGUSR1/SIGUSR2 to random workers\n");
    printf("- Cancel some workers mid-execution\n");
    printf("- Demonstrate cleanup handlers\n");
    printf("- Show signal masking per thread\n");
    printf("\nPress Ctrl+C to stop early\n");
    printf("===========================================\n\n");

    srand(time(NULL));

    atomic_store(&g_state.shutdown_flag, 0);
    atomic_store(&g_state.total_work, 0);
    apth_mutex_init(&g_state.print_mutex, NULL);

    g_state.workers = calloc(g_state.num_workers, sizeof(worker_t));

    for (int i = 0; i < g_state.num_workers; i++) {
        g_state.workers[i].id = i;
        atomic_store(&g_state.workers[i].work_done, 0);
        atomic_store(&g_state.workers[i].cancelled, 0);
        atomic_store(&g_state.workers[i].sigusr1_count, 0);
        atomic_store(&g_state.workers[i].sigusr2_count, 0);
    }

    signal(SIGINT, shutdown_handler);
    signal(SIGTERM, shutdown_handler);

    apth_t controller, stats, sig_monitor;

    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (int i = 0; i < g_state.num_workers; i++) {
        if (apth_create(&g_state.workers[i].thread, NULL, worker_thread,
                        &g_state.workers[i]) != 0) {
            fprintf(stderr, "Failed to create worker thread %d\n", i);
            exit(1);
        }
    }

    if (apth_create(&controller, NULL, controller_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create controller thread\n");
        exit(1);
    }

    if (apth_create(&stats, NULL, stats_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create stats thread\n");
        exit(1);
    }

    if (apth_create(&sig_monitor, NULL, signal_monitor_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create signal monitor thread\n");
        exit(1);
    }

    for (int i = 0; i < g_state.num_workers; i++) {
        void *retval;
        apth_join(g_state.workers[i].thread, &retval);
        if (retval == APTH_CANCELED) {
            safe_print("[Main] Worker %d was cancelled\n", i);
        }
    }

    apth_join(controller, NULL);
    apth_join(stats, NULL);
    apth_cancel(sig_monitor);
    apth_join(sig_monitor, NULL);

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                     (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    printf("\n===========================================\n");
    printf("Results\n");
    printf("===========================================\n");
    printf("Total work:      %lu\n", atomic_load(&g_state.total_work));
    printf("Elapsed time:    %.2f seconds\n", elapsed);
    printf("Work rate:       %.2f units/sec\n", atomic_load(&g_state.total_work) / elapsed);
    printf("\nPer-worker statistics:\n");
    for (int i = 0; i < g_state.num_workers; i++) {
        printf("  Worker %d: %lu work units, SIGUSR1: %d, SIGUSR2: %d, %s\n",
               i,
               atomic_load(&g_state.workers[i].work_done),
               atomic_load(&g_state.workers[i].sigusr1_count),
               atomic_load(&g_state.workers[i].sigusr2_count),
               atomic_load(&g_state.workers[i].cancelled) ? "CANCELLED" : "completed");
    }
    printf("===========================================\n");

    apth_mutex_destroy(&g_state.print_mutex);
    free(g_state.workers);

    exit(0);
}
APTH_MAIN_END
