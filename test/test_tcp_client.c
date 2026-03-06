/*
 * test_tcp_client.c
 *
 * Multi-threaded TCP echo client (pthread, no apth / no LD_PRELOAD).
 *
 * This is one half of the multi-process TCP client-server test pair.
 * Compile and run together with test_tcp_server via the Makefile target
 * `run-tcp-test`.
 *
 * Protocol
 * --------
 *   Spawns N_CLIENTS pthreads, each of which:
 *     1. Connects to the server.
 *     2. Sends MSG_LEN bytes (tagged with thread id mod 256).
 *     3. Reads the echo and verifies it.
 *     4. Repeats N_ROUNDS times.
 *     5. Closes the connection.
 *
 * Exit code
 * ---------
 *   0 on success, 1 on failure.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif // _GNU_SOURCE
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

/* ---- tunables (must match test_tcp_server.c) ---- */
#define SERVER_PORT   17777
#define N_CLIENTS     4
#define MSG_LEN       1024
#define N_ROUNDS      16

/* ---- globals ---- */
static _Atomic int g_pass = 1;

/* ------------------------------------------------------------------ */
static void *client_func(void *arg)
{
    int id = (int)(intptr_t)arg;
    unsigned char fill = (unsigned char)(id & 0xFF);
    char sbuf[MSG_LEN];
    char rbuf[MSG_LEN];
    memset(sbuf, fill, sizeof(sbuf));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[FAIL] client[%d]: socket: %s\n", id, strerror(errno));
        atomic_store(&g_pass, 0);
        return NULL;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(SERVER_PORT);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[FAIL] client[%d]: connect: %s\n", id, strerror(errno));
        atomic_store(&g_pass, 0);
        close(fd);
        return NULL;
    }

    for (int r = 0; r < N_ROUNDS; r++) {
        /* Send */
        size_t sent = 0;
        while (sent < MSG_LEN) {
            ssize_t rv = write(fd, sbuf + sent, MSG_LEN - sent);
            if (rv <= 0) {
                fprintf(stderr,
                        "[FAIL] client[%d]: write returned %zd at round %d\n",
                        id, rv, r);
                atomic_store(&g_pass, 0);
                close(fd);
                return NULL;
            }
            sent += (size_t)rv;
        }
        /* Receive echo */
        size_t got = 0;
        while (got < MSG_LEN) {
            ssize_t rv = read(fd, rbuf + got, MSG_LEN - got);
            if (rv <= 0) {
                fprintf(stderr,
                        "[FAIL] client[%d]: read returned %zd at round %d\n",
                        id, rv, r);
                atomic_store(&g_pass, 0);
                close(fd);
                return NULL;
            }
            got += (size_t)rv;
        }
        /* Verify */
        for (size_t i = 0; i < MSG_LEN; i++) {
            if ((unsigned char)rbuf[i] != fill) {
                fprintf(stderr,
                        "[FAIL] client[%d]: echo corruption at round %d "
                        "offset %zu (got 0x%02x)\n",
                        id, r, i, (unsigned char)rbuf[i]);
                atomic_store(&g_pass, 0);
                close(fd);
                return NULL;
            }
        }
    }
    close(fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
int main(void)
{
    pthread_t threads[N_CLIENTS];
    for (int i = 0; i < N_CLIENTS; i++)
        pthread_create(&threads[i], NULL, client_func, (void *)(intptr_t)i);
    for (int i = 0; i < N_CLIENTS; i++)
        pthread_join(threads[i], NULL);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_tcp_client\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_tcp_client\n");
    return 1;
}
