/*
 * http_server.c - Multi-threaded HTTP Web Server using LIBAPTH
 *
 * A comprehensive test application that exercises:
 * - Socket I/O (accept, recv, send)
 * - File I/O (serving static files)
 * - Thread pool with work queue
 * - Synchronization primitives (mutex, condition variables, rwlock)
 * - Signal handling for graceful shutdown
 * - Concurrent request handling
 *
 * Usage: ./http_server [port] [num_workers] [www_root]
 *   port        - Port to listen on (default: 8080)
 *   num_workers - Number of worker threads (default: 4)
 *   www_root    - Root directory for serving files (default: ./www)
 */

#define _GNU_SOURCE
#include "apth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <time.h>
#include <stdatomic.h>
#include <signal.h>

/* Configuration */
#define DEFAULT_PORT 8080
#define DEFAULT_WORKERS 4
#define DEFAULT_WWW_ROOT "./www"
#define MAX_QUEUE_SIZE 128
#define BUFFER_SIZE 8192
#define MAX_PATH 1024

/* Global state */
typedef struct {
    int client_fd;
    struct sockaddr_in client_addr;
} work_item_t;

typedef struct {
    work_item_t items[MAX_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    apth_mutex_t mutex;
    apth_cond_t not_empty;
    apth_cond_t not_full;
} work_queue_t;

typedef struct {
    int port;
    int num_workers;
    char www_root[MAX_PATH];
    int listen_fd;
    work_queue_t queue;
    _Atomic int shutdown_flag;
    _Atomic unsigned long total_requests;
    _Atomic unsigned long total_bytes_sent;
    apth_rwlock_t stats_lock;
} server_state_t;

static server_state_t g_server;

/* Work queue operations */
void queue_init(work_queue_t *q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    apth_mutex_init(&q->mutex, NULL);
    apth_cond_init(&q->not_empty, NULL);
    apth_cond_init(&q->not_full, NULL);
}

void queue_destroy(work_queue_t *q) {
    apth_mutex_destroy(&q->mutex);
    apth_cond_destroy(&q->not_empty);
    apth_cond_destroy(&q->not_full);
}

int queue_push(work_queue_t *q, work_item_t item) {
    apth_mutex_lock(&q->mutex);

    while (q->count >= MAX_QUEUE_SIZE && !atomic_load(&g_server.shutdown_flag)) {
        apth_cond_wait(&q->not_full, &q->mutex);
    }

    if (atomic_load(&g_server.shutdown_flag)) {
        apth_mutex_unlock(&q->mutex);
        return -1;
    }

    q->items[q->tail] = item;
    q->tail = (q->tail + 1) % MAX_QUEUE_SIZE;
    q->count++;

    apth_cond_signal(&q->not_empty);
    apth_mutex_unlock(&q->mutex);
    return 0;
}

int queue_pop(work_queue_t *q, work_item_t *item) {
    apth_mutex_lock(&q->mutex);

    while (q->count == 0 && !atomic_load(&g_server.shutdown_flag)) {
        apth_cond_wait(&q->not_empty, &q->mutex);
    }

    if (q->count == 0 && atomic_load(&g_server.shutdown_flag)) {
        apth_mutex_unlock(&q->mutex);
        return -1;
    }

    *item = q->items[q->head];
    q->head = (q->head + 1) % MAX_QUEUE_SIZE;
    q->count--;

    apth_cond_signal(&q->not_full);
    apth_mutex_unlock(&q->mutex);
    return 0;
}

/* HTTP response helpers */
const char *get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".txt") == 0) return "text/plain";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".pdf") == 0) return "application/pdf";

    return "application/octet-stream";
}

void send_response(int fd, int status_code, const char *status_text,
                   const char *content_type, const char *body, size_t body_len) {
    char header[BUFFER_SIZE];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Server: LIBAPTH-HTTP/1.0\r\n"
        "\r\n",
        status_code, status_text, content_type, body_len);

    send(fd, header, header_len, 0);
    if (body && body_len > 0) {
        send(fd, body, body_len, 0);
    }
}

void send_error(int fd, int status_code, const char *status_text, const char *message) {
    char body[BUFFER_SIZE];
    int body_len = snprintf(body, sizeof(body),
        "<html><head><title>%d %s</title></head>"
        "<body><h1>%d %s</h1><p>%s</p></body></html>",
        status_code, status_text, status_code, status_text, message);

    send_response(fd, status_code, status_text, "text/html", body, body_len);
}

/* Serve a file from disk */
void serve_file(int client_fd, const char *filepath) {
    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) {
        if (errno == ENOENT) {
            send_error(client_fd, 404, "Not Found", "The requested file was not found.");
        } else {
            send_error(client_fd, 500, "Internal Server Error", "Failed to open file.");
        }
        return;
    }

    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        send_error(client_fd, 500, "Internal Server Error", "Failed to stat file.");
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        close(file_fd);
        char index_path[MAX_PATH];
        snprintf(index_path, sizeof(index_path), "%s/index.html", filepath);
        serve_file(client_fd, index_path);
        return;
    }

    const char *mime_type = get_mime_type(filepath);
    char header[BUFFER_SIZE];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "Server: LIBAPTH-HTTP/1.0\r\n"
        "\r\n",
        mime_type, st.st_size);

    send(client_fd, header, header_len, 0);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    size_t total_sent = 0;
    while ((bytes_read = read(file_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t sent = send(client_fd, buffer, bytes_read, 0);
        if (sent > 0) {
            total_sent += sent;
        }
    }

    close(file_fd);
    atomic_fetch_add(&g_server.total_bytes_sent, total_sent);
}

/* Handle HTTP request */
void handle_request(int client_fd, struct sockaddr_in *client_addr) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (bytes_read <= 0) {
        close(client_fd);
        return;
    }

    buffer[bytes_read] = '\0';

    char method[16], path[MAX_PATH], version[16];
    if (sscanf(buffer, "%15s %1023s %15s", method, path, version) != 3) {
        send_error(client_fd, 400, "Bad Request", "Malformed HTTP request.");
        close(client_fd);
        return;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr->sin_addr, client_ip, sizeof(client_ip));
    printf("[%s] %s %s\n", client_ip, method, path);

    if (strcmp(method, "GET") != 0) {
        send_error(client_fd, 405, "Method Not Allowed", "Only GET method is supported.");
        close(client_fd);
        return;
    }

    if (strstr(path, "..") != NULL) {
        send_error(client_fd, 403, "Forbidden", "Path traversal not allowed.");
        close(client_fd);
        return;
    }

    char filepath[MAX_PATH];
    if (strcmp(path, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "%s/index.html", g_server.www_root);
    } else {
        snprintf(filepath, sizeof(filepath), "%s%s", g_server.www_root, path);
    }

    serve_file(client_fd, filepath);
    close(client_fd);

    atomic_fetch_add(&g_server.total_requests, 1);
}

/* Worker thread function */
void *worker_thread(void *arg) {
    int worker_id = *(int *)arg;
    printf("[Worker %d] Started\n", worker_id);

    while (!atomic_load(&g_server.shutdown_flag)) {
        work_item_t item;
        if (queue_pop(&g_server.queue, &item) == 0) {
            handle_request(item.client_fd, &item.client_addr);
        }
    }

    printf("[Worker %d] Shutting down\n", worker_id);
    return NULL;
}

/* Acceptor thread function */
void *acceptor_thread(void *arg) {
    (void)arg;
    printf("[Acceptor] Started on port %d\n", g_server.port);

    while (!atomic_load(&g_server.shutdown_flag)) {
        work_item_t item;
        socklen_t addr_len = sizeof(item.client_addr);

        item.client_fd = accept(g_server.listen_fd,
                                (struct sockaddr *)&item.client_addr,
                                &addr_len);

        if (item.client_fd < 0) {
            if (errno == EINTR || atomic_load(&g_server.shutdown_flag)) {
                break;
            }
            perror("accept");
            continue;
        }

        if (queue_push(&g_server.queue, item) < 0) {
            close(item.client_fd);
            break;
        }
    }

    printf("[Acceptor] Shutting down\n");
    return NULL;
}

/* Statistics thread function */
void *stats_thread(void *arg) {
    (void)arg;
    printf("[Stats] Started\n");

    while (!atomic_load(&g_server.shutdown_flag)) {
        sleep(10);
        if (atomic_load(&g_server.shutdown_flag)) break;

        unsigned long requests = atomic_load(&g_server.total_requests);
        unsigned long bytes = atomic_load(&g_server.total_bytes_sent);

        printf("[Stats] Requests: %lu, Bytes sent: %lu (%.2f MB)\n",
               requests, bytes, bytes / (1024.0 * 1024.0));
    }

    printf("[Stats] Shutting down\n");
    return NULL;
}

/* Signal handler for graceful shutdown */
void signal_handler(int signo) {
    (void)signo;
    printf("\n[Server] Received shutdown signal\n");
    atomic_store(&g_server.shutdown_flag, 1);

    if (g_server.listen_fd >= 0) {
        close(g_server.listen_fd);
    }

    apth_cond_broadcast(&g_server.queue.not_empty);
    apth_cond_broadcast(&g_server.queue.not_full);
}

/* Initialize server */
int server_init(int port, int num_workers, const char *www_root) {
    memset(&g_server, 0, sizeof(g_server));
    g_server.port = port;
    g_server.num_workers = num_workers;
    strncpy(g_server.www_root, www_root, sizeof(g_server.www_root) - 1);
    atomic_store(&g_server.shutdown_flag, 0);
    atomic_store(&g_server.total_requests, 0);
    atomic_store(&g_server.total_bytes_sent, 0);

    apth_rwlock_init(&g_server.stats_lock, NULL);
    queue_init(&g_server.queue);

    g_server.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server.listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(g_server.listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(g_server.listen_fd);
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(g_server.listen_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        perror("bind");
        close(g_server.listen_fd);
        return -1;
    }

    if (listen(g_server.listen_fd, 128) < 0) {
        perror("listen");
        close(g_server.listen_fd);
        return -1;
    }

    return 0;
}

/* Main application entry point */
APTH_CONFIG(cfg, cfg->workers = 4;)

APTH_MAIN_BEGIN(argc, argv)
{
    int port = DEFAULT_PORT;
    int num_workers = DEFAULT_WORKERS;
    char www_root[MAX_PATH];
    strncpy(www_root, DEFAULT_WWW_ROOT, sizeof(www_root) - 1);

    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) num_workers = atoi(argv[2]);
    if (argc > 3) strncpy(www_root, argv[3], sizeof(www_root) - 1);

    printf("===========================================\n");
    printf("LIBAPTH HTTP Server\n");
    printf("===========================================\n");
    printf("Port:         %d\n", port);
    printf("Workers:      %d\n", num_workers);
    printf("WWW Root:     %s\n", www_root);
    printf("===========================================\n\n");

    if (server_init(port, num_workers, www_root) < 0) {
        fprintf(stderr, "Failed to initialize server\n");
        exit(1);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    apth_t acceptor;
    apth_t workers[num_workers];
    apth_t stats;
    int worker_ids[num_workers];

    if (apth_create(&acceptor, NULL, acceptor_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create acceptor thread\n");
        exit(1);
    }

    for (int i = 0; i < num_workers; i++) {
        worker_ids[i] = i;
        if (apth_create(&workers[i], NULL, worker_thread, &worker_ids[i]) != 0) {
            fprintf(stderr, "Failed to create worker thread %d\n", i);
            exit(1);
        }
    }

    if (apth_create(&stats, NULL, stats_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create stats thread\n");
        exit(1);
    }

    printf("[Server] Ready to accept connections\n\n");

    apth_join(acceptor, NULL);

    for (int i = 0; i < num_workers; i++) {
        apth_join(workers[i], NULL);
    }

    apth_join(stats, NULL);

    queue_destroy(&g_server.queue);
    apth_rwlock_destroy(&g_server.stats_lock);

    printf("\n[Server] Shutdown complete\n");
    printf("Total requests: %lu\n", atomic_load(&g_server.total_requests));
    printf("Total bytes sent: %lu (%.2f MB)\n",
           atomic_load(&g_server.total_bytes_sent),
           atomic_load(&g_server.total_bytes_sent) / (1024.0 * 1024.0));

    exit(0);
}
APTH_MAIN_END

