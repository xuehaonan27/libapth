/*
 * test_socketpair_pthread.c
 *
 * Pthread baseline for test_socketpair_apth.c.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif // _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <stdatomic.h>

#define MSG_SIZE 1024
#define N_MSGS 10

static int g_sockpair[2];
static _Atomic int g_pass = 1;

static void *sender_func(void *arg)
{
    (void)arg;
    char buf[MSG_SIZE];

    for (int i = 0; i < N_MSGS; i++) {
        memset(buf, 0x50 + i, MSG_SIZE);
        ssize_t rv = send(g_sockpair[0], buf, MSG_SIZE, 0);
        if (rv != MSG_SIZE) {
            fprintf(stderr, "[FAIL] send returned %zd, expected %d\n",
                    rv, MSG_SIZE);
            atomic_store(&g_pass, 0);
            return NULL;
        }
    }
    return NULL;
}

static void *receiver_func(void *arg)
{
    (void)arg;
    char buf[MSG_SIZE];

    for (int i = 0; i < N_MSGS; i++) {
        ssize_t rv = recv(g_sockpair[1], buf, MSG_SIZE, 0);
        if (rv != MSG_SIZE) {
            fprintf(stderr, "[FAIL] recv returned %zd, expected %d\n",
                    rv, MSG_SIZE);
            atomic_store(&g_pass, 0);
            return NULL;
        }

        /* Verify data */
        unsigned char expected = 0x50 + i;
        for (int j = 0; j < MSG_SIZE; j++) {
            if ((unsigned char)buf[j] != expected) {
                fprintf(stderr, "[FAIL] data mismatch at msg %d, byte %d\n", i, j);
                atomic_store(&g_pass, 0);
                return NULL;
            }
        }
    }
    return NULL;
}

int main(void)
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, g_sockpair) != 0) {
        perror("socketpair");
        return 1;
    }

    pthread_t sender, receiver;
    pthread_create(&sender, NULL, sender_func, NULL);
    pthread_create(&receiver, NULL, receiver_func, NULL);

    pthread_join(sender, NULL);
    pthread_join(receiver, NULL);

    close(g_sockpair[0]);
    close(g_sockpair[1]);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_socketpair_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_socketpair_pthread\n");
    return 1;
}
