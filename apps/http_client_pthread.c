/*
 * http_client.c - Multi-threaded HTTP Client using LIBPTHREAD
 *
 * A load testing client that exercises:
 * - Socket I/O (connect, send, recv)
 * - Multiple concurrent connections
 * - Thread synchronization (barrier, mutex)
 * - Statistics collection
 *
 * Usage: ./http_client <host> <port> <num_threads> <requests_per_thread>
 *   host                - Server hostname or IP (default: localhost)
 *   port                - Server port (default: 8080)
 *   num_threads         - Number of concurrent threads (default: 10)
 *   requests_per_thread - Requests per thread (default: 100)
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <stdatomic.h>

/* Configuration */
#define DEFAULT_HOST "localhost"
#define DEFAULT_PORT 8080
#define DEFAULT_THREADS 10
#define DEFAULT_REQUESTS 100
#define BUFFER_SIZE 8192

/* Statistics */
typedef struct {
    _Atomic unsigned long total_requests;
    _Atomic unsigned long successful_requests;
    _Atomic unsigned long failed_requests;
    _Atomic unsigned long total_bytes_received;
    pthread_mutex_t mutex;
    pthread_barrier_t start_barrier;
    struct timespec start_time;
    struct timespec end_time;
} stats_t;

/* Client configuration */
typedef struct {
    char host[256];
    int port;
    int num_threads;
    int requests_per_thread;
    stats_t stats;
} client_config_t;

static client_config_t g_config;

/* Connect to server */
int connect_to_server(const char *host, int port) {
    struct hostent *server = gethostbyname(host);
    if (server == NULL) {
        fprintf(stderr, "Error: No such host %s\n", host);
        return -1;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(port);

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/* Send HTTP request and receive response */
int send_request(int sockfd, const char *path) {
    char request[BUFFER_SIZE];
    int request_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: LIBAPTH-Client/1.0\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, g_config.host, g_config.port);

    if (send(sockfd, request, request_len, 0) < 0) {
        perror("send");
        return -1;
    }

    char buffer[BUFFER_SIZE];
    ssize_t total_received = 0;
    ssize_t bytes_received;

    while ((bytes_received = recv(sockfd, buffer, sizeof(buffer), 0)) > 0) {
        total_received += bytes_received;
    }

    if (bytes_received < 0) {
        perror("recv");
        return -1;
    }

    return total_received;
}

/* Worker thread function */
void *client_worker(void *arg) {
    int thread_id = *(int *)arg;

    pthread_barrier_wait(&g_config.stats.start_barrier);

    for (int i = 0; i < g_config.requests_per_thread; i++) {
        int sockfd = connect_to_server(g_config.host, g_config.port);
        if (sockfd < 0) {
            atomic_fetch_add(&g_config.stats.failed_requests, 1);
            continue;
        }

        int bytes = send_request(sockfd, "/");
        close(sockfd);

        if (bytes > 0) {
            atomic_fetch_add(&g_config.stats.successful_requests, 1);
            atomic_fetch_add(&g_config.stats.total_bytes_received, bytes);
        } else {
            atomic_fetch_add(&g_config.stats.failed_requests, 1);
        }

        atomic_fetch_add(&g_config.stats.total_requests, 1);

        if ((i + 1) % 10 == 0) {
            printf("[Thread %d] Completed %d/%d requests\n",
                   thread_id, i + 1, g_config.requests_per_thread);
        }
    }

    printf("[Thread %d] Finished\n", thread_id);
    return NULL;
}

int main(int argc, char *argv[])
{
    strncpy(g_config.host, DEFAULT_HOST, sizeof(g_config.host) - 1);
    g_config.port = DEFAULT_PORT;
    g_config.num_threads = DEFAULT_THREADS;
    g_config.requests_per_thread = DEFAULT_REQUESTS;

    if (argc > 1) strncpy(g_config.host, argv[1], sizeof(g_config.host) - 1);
    if (argc > 2) g_config.port = atoi(argv[2]);
    if (argc > 3) g_config.num_threads = atoi(argv[3]);
    if (argc > 4) g_config.requests_per_thread = atoi(argv[4]);

    printf("===========================================\n");
    printf("LIBAPTH HTTP Client\n");
    printf("===========================================\n");
    printf("Host:                %s\n", g_config.host);
    printf("Port:                %d\n", g_config.port);
    printf("Threads:             %d\n", g_config.num_threads);
    printf("Requests per thread: %d\n", g_config.requests_per_thread);
    printf("Total requests:      %d\n",
           g_config.num_threads * g_config.requests_per_thread);
    printf("===========================================\n\n");

    atomic_store(&g_config.stats.total_requests, 0);
    atomic_store(&g_config.stats.successful_requests, 0);
    atomic_store(&g_config.stats.failed_requests, 0);
    atomic_store(&g_config.stats.total_bytes_received, 0);

    pthread_mutex_init(&g_config.stats.mutex, NULL);
    pthread_barrier_init(&g_config.stats.start_barrier, NULL, g_config.num_threads + 1);

    pthread_t threads[g_config.num_threads];
    int thread_ids[g_config.num_threads];

    for (int i = 0; i < g_config.num_threads; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, client_worker, &thread_ids[i]) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            exit(1);
        }
    }

    printf("[Client] Starting load test...\n\n");
    clock_gettime(CLOCK_MONOTONIC, &g_config.stats.start_time);

    pthread_barrier_wait(&g_config.stats.start_barrier);

    for (int i = 0; i < g_config.num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &g_config.stats.end_time);

    double elapsed = (g_config.stats.end_time.tv_sec - g_config.stats.start_time.tv_sec) +
                     (g_config.stats.end_time.tv_nsec - g_config.stats.start_time.tv_nsec) / 1e9;

    printf("\n===========================================\n");
    printf("Test Results\n");
    printf("===========================================\n");
    printf("Total requests:      %lu\n", atomic_load(&g_config.stats.total_requests));
    printf("Successful:          %lu\n", atomic_load(&g_config.stats.successful_requests));
    printf("Failed:              %lu\n", atomic_load(&g_config.stats.failed_requests));
    printf("Bytes received:      %lu (%.2f MB)\n",
           atomic_load(&g_config.stats.total_bytes_received),
           atomic_load(&g_config.stats.total_bytes_received) / (1024.0 * 1024.0));
    printf("Elapsed time:        %.2f seconds\n", elapsed);
    printf("Requests per second: %.2f\n",
           atomic_load(&g_config.stats.successful_requests) / elapsed);
    printf("===========================================\n");

    pthread_mutex_destroy(&g_config.stats.mutex);
    pthread_barrier_destroy(&g_config.stats.start_barrier);

    exit(0);
}
