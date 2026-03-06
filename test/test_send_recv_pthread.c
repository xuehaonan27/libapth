/*
 * test_send_recv_pthread.c
 *
 * Pthread baseline for test_send_recv_apth.c.
 * Same send/recv scenario using pthreads and real libc calls.
 *
 * Pass criteria
 * -------------
 *   Receiver accumulates exactly TOTAL_DATA bytes, all equal to 0xCD.
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
#define CHUNK_SIZE  2048
#define N_CHUNKS    64
#define TOTAL_DATA  ((size_t)(CHUNK_SIZE) * (size_t)(N_CHUNKS))   /* 128 KB */
#define FILL_BYTE   0xCD

/* ---- globals ---- */
static int g_fds[2];
static _Atomic int g_pass = 1;

/* ------------------------------------------------------------------ */
static void *sender_func(void *arg)
{
    (void)arg;
    char buf[CHUNK_SIZE];
    memset(buf, FILL_BYTE, sizeof(buf));

    for (int i = 0; i < N_CHUNKS; i++) {
        ssize_t rv = send(g_fds[0], buf, CHUNK_SIZE, 0);
        if (rv != CHUNK_SIZE) {
            fprintf(stderr,
                    "[FAIL] sender: send() returned %zd, expected %d\n",
                    rv, CHUNK_SIZE);
            atomic_store(&g_pass, 0);
            return NULL;
        }
    }
    return NULL;
}

static void *receiver_func(void *arg)
{
    (void)arg;
    char buf[CHUNK_SIZE];
    size_t total = 0;

    while (total < TOTAL_DATA) {
        size_t want = TOTAL_DATA - total;
        if (want > CHUNK_SIZE) want = CHUNK_SIZE;

        ssize_t rv = recv(g_fds[1], buf, want, 0);
        if (rv == 0) {
            fprintf(stderr, "[FAIL] receiver: unexpected EOF at %zu bytes\n", total);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        if (rv < 0) {
            fprintf(stderr, "[FAIL] receiver: recv() returned %zd\n", rv);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        for (ssize_t i = 0; i < rv; i++) {
            if ((unsigned char)buf[i] != FILL_BYTE) {
                fprintf(stderr,
                        "[FAIL] receiver: corruption at byte %zu (got 0x%02x)\n",
                        total + (size_t)i, (unsigned char)buf[i]);
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
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, g_fds) != 0) {
        perror("socketpair");
        return 1;
    }

    pthread_t sender, receiver;
    pthread_create(&sender,   NULL, sender_func,   NULL);
    pthread_create(&receiver, NULL, receiver_func, NULL);

    pthread_join(sender,   NULL);
    pthread_join(receiver, NULL);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_send_recv_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_send_recv_pthread\n");
    return 1;
}
