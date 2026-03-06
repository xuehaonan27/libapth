/*
 * test_cs_unix_pthread.c
 *
 * Pthread baseline for test_cs_unix_apth.c.
 * Same combined client-server scenario using pthreads and real libc.
 *
 * Pass criteria
 * -------------
 *   All N_PAIRS × N_ROUNDS request/response cycles complete without error
 *   or data corruption.  Process exits 0 on pass, 1 on fail.
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
#include <sys/types.h>
#include <sys/socket.h>

/* ---- tunables ---- */
#define N_PAIRS     8
#define REQ_LEN     512
#define N_ROUNDS    32

/* ---- globals ---- */
static int g_fds[N_PAIRS][2];
static _Atomic int g_pass = 1;

/* ------------------------------------------------------------------ */
static void *server_func(void *arg)
{
    int idx = (int)(intptr_t)arg;
    unsigned char fill = (unsigned char)(idx & 0xFF);
    char buf[REQ_LEN];

    for (int r = 0; r < N_ROUNDS; r++) {
        size_t got = 0;
        while (got < REQ_LEN) {
            ssize_t rv = recv(g_fds[idx][0], buf + got, REQ_LEN - got, 0);
            if (rv <= 0) {
                fprintf(stderr,
                        "[FAIL] server[%d]: recv() returned %zd at round %d\n",
                        idx, rv, r);
                atomic_store(&g_pass, 0);
                return NULL;
            }
            got += (size_t)rv;
        }
        for (size_t i = 0; i < REQ_LEN; i++) {
            if ((unsigned char)buf[i] != fill) {
                fprintf(stderr,
                        "[FAIL] server[%d]: request corruption at round %d "
                        "offset %zu\n", idx, r, i);
                atomic_store(&g_pass, 0);
                return NULL;
            }
        }
        ssize_t rv = write(g_fds[idx][0], buf, REQ_LEN);
        if (rv != REQ_LEN) {
            fprintf(stderr,
                    "[FAIL] server[%d]: write() returned %zd at round %d\n",
                    idx, rv, r);
            atomic_store(&g_pass, 0);
            return NULL;
        }
    }
    return NULL;
}

static void *client_func(void *arg)
{
    int idx = (int)(intptr_t)arg;
    unsigned char fill = (unsigned char)(idx & 0xFF);
    char buf[REQ_LEN];
    memset(buf, fill, sizeof(buf));

    for (int r = 0; r < N_ROUNDS; r++) {
        ssize_t sv = send(g_fds[idx][1], buf, REQ_LEN, 0);
        if (sv != REQ_LEN) {
            fprintf(stderr,
                    "[FAIL] client[%d]: send() returned %zd at round %d\n",
                    idx, sv, r);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        char rbuf[REQ_LEN];
        size_t got = 0;
        while (got < REQ_LEN) {
            ssize_t rv = read(g_fds[idx][1], rbuf + got, REQ_LEN - got);
            if (rv <= 0) {
                fprintf(stderr,
                        "[FAIL] client[%d]: read() returned %zd at round %d\n",
                        idx, rv, r);
                atomic_store(&g_pass, 0);
                return NULL;
            }
            got += (size_t)rv;
        }
        for (size_t i = 0; i < REQ_LEN; i++) {
            if ((unsigned char)rbuf[i] != fill) {
                fprintf(stderr,
                        "[FAIL] client[%d]: echo corruption at round %d "
                        "offset %zu\n", idx, r, i);
                atomic_store(&g_pass, 0);
                return NULL;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
int main(void)
{
    for (int i = 0; i < N_PAIRS; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, g_fds[i]) != 0) {
            perror("socketpair");
            return 1;
        }
    }

    pthread_t servers[N_PAIRS], clients[N_PAIRS];
    for (int i = 0; i < N_PAIRS; i++) {
        pthread_create(&servers[i], NULL, server_func, (void *)(intptr_t)i);
        pthread_create(&clients[i], NULL, client_func, (void *)(intptr_t)i);
    }
    for (int i = 0; i < N_PAIRS; i++) {
        pthread_join(servers[i], NULL);
        pthread_join(clients[i], NULL);
    }

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_cs_unix_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_cs_unix_pthread\n");
    return 1;
}
