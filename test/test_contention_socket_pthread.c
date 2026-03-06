/*
 * test_contention_socket_pthread.c
 *
 * Pthread baseline for test_contention_socket_apth.c.
 * Same high-concurrency socket send/recv scenario using pthreads.
 *
 * Pass criteria
 * -------------
 *   All N_PAIRS pairs complete without data loss or corruption.
 *   Process exits 0 on pass, 1 on fail.
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
#define N_PAIRS          16
#define BYTES_PER_SENDER ((size_t)(8 * 1024))
#define SEND_CHUNK       512
#define RECV_CHUNK       512

/* ---- globals ---- */
static int g_fds[N_PAIRS][2];
static _Atomic int g_pass = 1;

/* ------------------------------------------------------------------ */
typedef struct { int pair_idx; } pair_arg_t;
static pair_arg_t g_args[N_PAIRS];

static void *sender_func(void *arg)
{
    int idx = ((pair_arg_t *)arg)->pair_idx;
    unsigned char fill = (unsigned char)(idx & 0xFF);
    char buf[SEND_CHUNK];
    memset(buf, fill, sizeof(buf));

    size_t remaining = BYTES_PER_SENDER;
    while (remaining > 0) {
        size_t chunk = remaining < SEND_CHUNK ? remaining : SEND_CHUNK;
        ssize_t rv = send(g_fds[idx][0], buf, chunk, 0);
        if (rv <= 0) {
            fprintf(stderr,
                    "[FAIL] sender[%d]: send() returned %zd\n", idx, rv);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        remaining -= (size_t)rv;
    }
    return NULL;
}

static void *receiver_func(void *arg)
{
    int idx = ((pair_arg_t *)arg)->pair_idx;
    unsigned char fill = (unsigned char)(idx & 0xFF);
    char buf[RECV_CHUNK];
    size_t total = 0;

    while (total < BYTES_PER_SENDER) {
        size_t want = BYTES_PER_SENDER - total;
        if (want > RECV_CHUNK) want = RECV_CHUNK;

        ssize_t rv = recv(g_fds[idx][1], buf, want, 0);
        if (rv == 0) {
            fprintf(stderr,
                    "[FAIL] receiver[%d]: unexpected EOF at %zu bytes\n",
                    idx, total);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        if (rv < 0) {
            fprintf(stderr,
                    "[FAIL] receiver[%d]: recv() returned %zd\n", idx, rv);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        for (ssize_t i = 0; i < rv; i++) {
            if ((unsigned char)buf[i] != fill) {
                fprintf(stderr,
                        "[FAIL] receiver[%d]: byte corruption at %zu "
                        "(got 0x%02x, expected 0x%02x)\n",
                        idx, total + (size_t)i,
                        (unsigned char)buf[i], fill);
                atomic_store(&g_pass, 0);
                return NULL;
            }
        }
        total += (size_t)rv;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
int main(void)
{
    for (int i = 0; i < N_PAIRS; i++) {
        g_args[i].pair_idx = i;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, g_fds[i]) != 0) {
            perror("socketpair");
            return 1;
        }
    }

    pthread_t senders[N_PAIRS], receivers[N_PAIRS];
    for (int i = 0; i < N_PAIRS; i++) {
        pthread_create(&senders[i],   NULL, sender_func,   &g_args[i]);
        pthread_create(&receivers[i], NULL, receiver_func, &g_args[i]);
    }
    for (int i = 0; i < N_PAIRS; i++) {
        pthread_join(senders[i],   NULL);
        pthread_join(receivers[i], NULL);
    }

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_contention_socket_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_contention_socket_pthread\n");
    return 1;
}
