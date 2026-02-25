/*
 * test_tcp_server.c
 *
 * Multi-threaded TCP echo server (pthread, no apth / no LD_PRELOAD).
 *
 * This is one half of the multi-process TCP client-server test pair.
 * Compile and run together with test_tcp_client via the Makefile target
 * `run-tcp-test`.
 *
 * Protocol
 * --------
 *   The server accepts N_CLIENTS connections.  For each connection it:
 *     1. Reads MSG_LEN bytes.
 *     2. Echoes them back.
 *     3. Repeats N_ROUNDS times.
 *     4. Closes the connection.
 *   A dedicated pthread is spawned per accepted connection.
 *
 * Exit code
 * ---------
 *   0 on success (all connections handled without error), 1 on failure.
 *
 * Communication with client
 * -------------------------
 *   The server prints "READY\n" to stdout once the listen socket is bound
 *   so the client knows it is safe to connect.  The Makefile run target
 *   uses this to synchronise the two processes.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---- tunables (must match test_tcp_client.c) ---- */
#define SERVER_PORT   17777
#define N_CLIENTS     4
#define MSG_LEN       1024
#define N_ROUNDS      16

/* ---- globals ---- */
static _Atomic int g_pass = 1;

/* ------------------------------------------------------------------ */
static void *conn_handler(void *arg)
{
    int conn_fd = (int)(intptr_t)arg;
    char buf[MSG_LEN];

    for (int r = 0; r < N_ROUNDS; r++) {
        /* Read full request */
        size_t got = 0;
        while (got < MSG_LEN) {
            ssize_t rv = read(conn_fd, buf + got, MSG_LEN - got);
            if (rv <= 0) {
                fprintf(stderr,
                        "[FAIL] server handler: read returned %zd at round %d\n",
                        rv, r);
                atomic_store(&g_pass, 0);
                close(conn_fd);
                return NULL;
            }
            got += (size_t)rv;
        }
        /* Echo back */
        size_t sent = 0;
        while (sent < MSG_LEN) {
            ssize_t rv = write(conn_fd, buf + sent, MSG_LEN - sent);
            if (rv <= 0) {
                fprintf(stderr,
                        "[FAIL] server handler: write returned %zd at round %d\n",
                        rv, r);
                atomic_store(&g_pass, 0);
                close(conn_fd);
                return NULL;
            }
            sent += (size_t)rv;
        }
    }
    close(conn_fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
int main(void)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(SERVER_PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind"); return 1;
    }
    if (listen(listen_fd, N_CLIENTS + 4) != 0) {
        perror("listen"); return 1;
    }

    /* Tell the client we are ready */
    printf("READY\n");
    fflush(stdout);

    pthread_t threads[N_CLIENTS];
    for (int i = 0; i < N_CLIENTS; i++) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            fprintf(stderr, "[FAIL] server: accept failed: %s\n",
                    strerror(errno));
            return 1;
        }
        pthread_create(&threads[i], NULL, conn_handler, (void *)(intptr_t)conn_fd);
    }
    for (int i = 0; i < N_CLIENTS; i++)
        pthread_join(threads[i], NULL);

    close(listen_fd);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_tcp_server\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_tcp_server\n");
    return 1;
}
