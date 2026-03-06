/*
 * test_sendto_recvfrom_pthread.c
 *
 * Pthread baseline for test_sendto_recvfrom_apth.c.
 * Same datagram scenario using pthreads and real libc calls.
 *
 * Pass criteria
 * -------------
 *   All N_MESSAGES datagrams are received intact, in order.
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
#include <sys/un.h>

/* ---- tunables ---- */
#define N_MESSAGES   256
#define MSG_LEN      128

/* ---- globals ---- */
static int g_fds[2];
static _Atomic int g_pass = 1;

/* ------------------------------------------------------------------ */
static void *sender_func(void *arg)
{
    (void)arg;
    char buf[MSG_LEN];

    for (int i = 0; i < N_MESSAGES; i++) {
        memset(buf, 0xEF, sizeof(buf));
        buf[0] = (char)(i & 0xFF);

        ssize_t rv = sendto(g_fds[0], buf, MSG_LEN, 0, NULL, 0);
        if (rv != MSG_LEN) {
            fprintf(stderr,
                    "[FAIL] sender: sendto() returned %zd at msg %d\n",
                    rv, i);
            atomic_store(&g_pass, 0);
            return NULL;
        }
    }
    return NULL;
}

static void *receiver_func(void *arg)
{
    (void)arg;
    char buf[MSG_LEN];
    struct sockaddr_storage src_addr;
    socklen_t addrlen;

    for (int i = 0; i < N_MESSAGES; i++) {
        addrlen = sizeof(src_addr);
        ssize_t rv = recvfrom(g_fds[1], buf, sizeof(buf), 0,
                              (struct sockaddr *)&src_addr, &addrlen);
        if (rv != MSG_LEN) {
            fprintf(stderr,
                    "[FAIL] receiver: recvfrom() returned %zd at msg %d\n",
                    rv, i);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        if ((unsigned char)buf[0] != (unsigned char)(i & 0xFF)) {
            fprintf(stderr,
                    "[FAIL] receiver: seq byte mismatch at msg %d\n", i);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        for (int j = 1; j < MSG_LEN; j++) {
            if ((unsigned char)buf[j] != 0xEF) {
                fprintf(stderr,
                        "[FAIL] receiver: fill byte corruption at msg %d "
                        "offset %d (got 0x%02x)\n",
                        i, j, (unsigned char)buf[j]);
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
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, g_fds) != 0) {
        perror("socketpair(SOCK_DGRAM)");
        return 1;
    }

    pthread_t sender, receiver;
    pthread_create(&sender,   NULL, sender_func,   NULL);
    pthread_create(&receiver, NULL, receiver_func, NULL);

    pthread_join(sender,   NULL);
    pthread_join(receiver, NULL);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_sendto_recvfrom_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_sendto_recvfrom_pthread\n");
    return 1;
}
