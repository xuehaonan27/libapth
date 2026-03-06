/*
 * dining_philosophers.c - Dining Philosophers Problem using LIBAPTH
 *
 * Demonstrates:
 * - Deadlock avoidance strategies
 * - Mutex for fork synchronization
 * - Semaphore for limiting concurrent diners
 * - Thread lifecycle management
 * - Resource ordering to prevent deadlock
 *
 * Usage: ./dining_philosophers <num_philosophers> <meals_per_philosopher>
 *   num_philosophers       - Number of philosophers (default: 5)
 *   meals_per_philosopher  - Meals each philosopher eats (default: 10)
 */

#define _GNU_SOURCE
#include "apth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>
#include <stdarg.h>

/* Configuration */
#define DEFAULT_PHILOSOPHERS 5
#define DEFAULT_MEALS 10
#define MAX_PHILOSOPHERS 20

/* Fork (chopstick) */
typedef struct {
    apth_mutex_t mutex;
    int id;
    _Atomic int holder;
} fork_t;

/* Philosopher state */
typedef enum {
    THINKING,
    HUNGRY,
    EATING
} phil_state_t;

/* Philosopher */
typedef struct {
    int id;
    int meals_eaten;
    fork_t *left_fork;
    fork_t *right_fork;
    phil_state_t state;
    unsigned long think_time_us;
    unsigned long eat_time_us;
} philosopher_t;

/* Global state */
typedef struct {
    int num_philosophers;
    int meals_per_philosopher;
    fork_t *forks;
    philosopher_t *philosophers;
    apth_sem_t table_sem;
    apth_mutex_t print_mutex;
    _Atomic unsigned long total_meals;
    _Atomic int deadlock_detected;
} app_state_t;

static app_state_t g_state;

/* Thread-safe printing */
void safe_print(const char *format, ...) {
    va_list args;
    va_start(args, format);
    apth_mutex_lock(&g_state.print_mutex);
    vprintf(format, args);
    fflush(stdout);
    apth_mutex_unlock(&g_state.print_mutex);
    va_end(args);
}

/* Initialize fork */
void fork_init(fork_t *fork, int id) {
    apth_mutex_init(&fork->mutex, NULL);
    fork->id = id;
    atomic_store(&fork->holder, -1);
}

void fork_destroy(fork_t *fork) {
    apth_mutex_destroy(&fork->mutex);
}

/* Pick up fork */
int fork_pickup(fork_t *fork, int phil_id) {
    if (apth_mutex_trylock(&fork->mutex) == 0) {
        atomic_store(&fork->holder, phil_id);
        return 1;
    }
    return 0;
}

void fork_putdown(fork_t *fork) {
    atomic_store(&fork->holder, -1);
    apth_mutex_unlock(&fork->mutex);
}

/* Philosopher actions */
void think(philosopher_t *phil) {
    phil->state = THINKING;
    safe_print("[Philosopher %d] Thinking...\n", phil->id);
    usleep((rand() % 100 + 50) * 1000);
    phil->think_time_us += (rand() % 100 + 50) * 1000;
}

void get_hungry(philosopher_t *phil) {
    phil->state = HUNGRY;
    safe_print("[Philosopher %d] Hungry, trying to pick up forks %d and %d\n",
               phil->id, phil->left_fork->id, phil->right_fork->id);
}

int try_eat(philosopher_t *phil) {
    apth_sem_wait(&g_state.table_sem);

    fork_t *first_fork = phil->left_fork;
    fork_t *second_fork = phil->right_fork;

    if (phil->id % 2 == 0) {
        first_fork = phil->right_fork;
        second_fork = phil->left_fork;
    }

    if (!fork_pickup(first_fork, phil->id)) {
        apth_sem_post(&g_state.table_sem);
        return 0;
    }

    usleep(1000);

    if (!fork_pickup(second_fork, phil->id)) {
        fork_putdown(first_fork);
        apth_sem_post(&g_state.table_sem);
        return 0;
    }

    phil->state = EATING;
    safe_print("[Philosopher %d] Eating (meal #%d)\n", phil->id, phil->meals_eaten + 1);

    usleep((rand() % 80 + 40) * 1000);
    phil->eat_time_us += (rand() % 80 + 40) * 1000;

    phil->meals_eaten++;
    atomic_fetch_add(&g_state.total_meals, 1);

    fork_putdown(second_fork);
    fork_putdown(first_fork);
    apth_sem_post(&g_state.table_sem);

    safe_print("[Philosopher %d] Finished eating, putting down forks\n", phil->id);
    return 1;
}

/* Philosopher thread */
void *philosopher_thread(void *arg) {
    philosopher_t *phil = (philosopher_t *)arg;

    safe_print("[Philosopher %d] Seated at the table\n", phil->id);

    while (phil->meals_eaten < g_state.meals_per_philosopher) {
        think(phil);

        get_hungry(phil);

        while (!try_eat(phil)) {
            usleep(10000);
            apth_yield();
        }

        apth_testcancel();
    }

    safe_print("[Philosopher %d] Satisfied, leaving the table (ate %d meals)\n",
               phil->id, phil->meals_eaten);

    return NULL;
}

/* Monitor thread */
void *monitor_thread(void *arg) {
    (void)arg;
    safe_print("[Monitor] Started\n");

    while (atomic_load(&g_state.total_meals) <
           (unsigned long)(g_state.num_philosophers * g_state.meals_per_philosopher)) {
        sleep(2);

        safe_print("[Monitor] Progress: %lu / %d meals completed\n",
                   atomic_load(&g_state.total_meals),
                   g_state.num_philosophers * g_state.meals_per_philosopher);

        apth_mutex_lock(&g_state.print_mutex);
        printf("[Monitor] Fork status: ");
        for (int i = 0; i < g_state.num_philosophers; i++) {
            int holder = atomic_load(&g_state.forks[i].holder);
            if (holder >= 0) {
                printf("F%d->P%d ", i, holder);
            } else {
                printf("F%d:free ", i);
            }
        }
        printf("\n");
        apth_mutex_unlock(&g_state.print_mutex);
    }

    safe_print("[Monitor] All meals completed\n");
    return NULL;
}

/* Main application entry point */
APTH_CONFIG(cfg, cfg->workers = 4;)

APTH_MAIN_BEGIN(argc, argv)
{
    g_state.num_philosophers = DEFAULT_PHILOSOPHERS;
    g_state.meals_per_philosopher = DEFAULT_MEALS;

    if (argc > 1) g_state.num_philosophers = atoi(argv[1]);
    if (argc > 2) g_state.meals_per_philosopher = atoi(argv[2]);

    if (g_state.num_philosophers > MAX_PHILOSOPHERS) {
        g_state.num_philosophers = MAX_PHILOSOPHERS;
    }
    if (g_state.num_philosophers < 2) {
        g_state.num_philosophers = 2;
    }

    printf("===========================================\n");
    printf("LIBAPTH Dining Philosophers\n");
    printf("===========================================\n");
    printf("Philosophers: %d\n", g_state.num_philosophers);
    printf("Meals each:   %d\n", g_state.meals_per_philosopher);
    printf("Total meals:  %d\n", g_state.num_philosophers * g_state.meals_per_philosopher);
    printf("===========================================\n\n");

    srand(time(NULL));

    atomic_store(&g_state.total_meals, 0);
    atomic_store(&g_state.deadlock_detected, 0);

    apth_sem_init(&g_state.table_sem, 0, g_state.num_philosophers - 1);
    apth_mutex_init(&g_state.print_mutex, NULL);

    g_state.forks = malloc(g_state.num_philosophers * sizeof(fork_t));
    g_state.philosophers = malloc(g_state.num_philosophers * sizeof(philosopher_t));

    for (int i = 0; i < g_state.num_philosophers; i++) {
        fork_init(&g_state.forks[i], i);
    }

    for (int i = 0; i < g_state.num_philosophers; i++) {
        g_state.philosophers[i].id = i;
        g_state.philosophers[i].meals_eaten = 0;
        g_state.philosophers[i].left_fork = &g_state.forks[i];
        g_state.philosophers[i].right_fork = &g_state.forks[(i + 1) % g_state.num_philosophers];
        g_state.philosophers[i].state = THINKING;
        g_state.philosophers[i].think_time_us = 0;
        g_state.philosophers[i].eat_time_us = 0;
    }

    apth_t phil_threads[g_state.num_philosophers];
    apth_t monitor;

    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (int i = 0; i < g_state.num_philosophers; i++) {
        if (apth_create(&phil_threads[i], NULL, philosopher_thread,
                        &g_state.philosophers[i]) != 0) {
            fprintf(stderr, "Failed to create philosopher thread %d\n", i);
            exit(1);
        }
    }

    if (apth_create(&monitor, NULL, monitor_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create monitor thread\n");
        exit(1);
    }

    for (int i = 0; i < g_state.num_philosophers; i++) {
        apth_join(phil_threads[i], NULL);
    }

    apth_join(monitor, NULL);

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                     (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    printf("\n===========================================\n");
    printf("Results\n");
    printf("===========================================\n");
    printf("Total meals:     %lu\n", atomic_load(&g_state.total_meals));
    printf("Elapsed time:    %.2f seconds\n", elapsed);
    printf("Meals per second: %.2f\n", atomic_load(&g_state.total_meals) / elapsed);
    printf("\nPer-philosopher statistics:\n");
    for (int i = 0; i < g_state.num_philosophers; i++) {
        printf("  Philosopher %d: %d meals, %.2fs thinking, %.2fs eating\n",
               i, g_state.philosophers[i].meals_eaten,
               g_state.philosophers[i].think_time_us / 1e6,
               g_state.philosophers[i].eat_time_us / 1e6);
    }
    printf("===========================================\n");

    for (int i = 0; i < g_state.num_philosophers; i++) {
        fork_destroy(&g_state.forks[i]);
    }

    apth_sem_destroy(&g_state.table_sem);
    apth_mutex_destroy(&g_state.print_mutex);
    free(g_state.forks);
    free(g_state.philosophers);

    exit(0);
}
APTH_MAIN_END
