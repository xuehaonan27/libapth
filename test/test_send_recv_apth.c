/*
 * test_send_recv_apth.c
 *
 * Test send(2) and recv(2) over a Unix-domain stream socket pair under libapth.
 *
 * Rationale for socketpair()
 * --------------------------
 *   apth's hooked socket() is currently unimplemented (TODO → abort).
 *   socketpair(2) is NOT hooked by apth, so it calls the real libc and gives
 *   us two connected socket fds that we can then use with the hooked send/recv.
 *
 * Scenario
 * --------
 *   • 1 sender apth sends TOTAL_DATA bytes (all 0xCD) via send() on fds[0].
 *   • 1 receiver apth loops calling recv() on fds[1] until TOTAL_DATA bytes
 *     are accumulated, verifying every byte equals 0xCD.
 *   • After both apths finish the main apth checks the result.
 *
 * Pass criteria
 * -------------
 *   Receiver accumulates exactly TOTAL_DATA bytes, all equal to 0xCD.
 *   Process exits 0 on pass, 1 on fail.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif // _GNU_SOURCE
#include "apth.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <sys/socket.h>

/* ---- tunables ---- */
#define N_WORKERS   2
#define CHUNK_SIZE  2048
#define N_CHUNKS    64
#define TOTAL_DATA  ((size_t)(CHUNK_SIZE) * (size_t)(N_CHUNKS))   /* 128 KB */
#define FILL_BYTE   0xCD

/* ---- globals ---- */
static int g_fds[2];          /* socketpair fds: sender→[0], receiver←[1] */
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
APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, g_fds) != 0) {
        perror("socketpair");
        exit(1);
    }

    apth_t sender, receiver;
    apth_create(&sender,   NULL, sender_func,   NULL);
    apth_create(&receiver, NULL, receiver_func, NULL);

    apth_join(sender,   NULL);
    apth_join(receiver, NULL);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_send_recv_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_send_recv_apth\n");
    exit(1);
}
APTH_MAIN_END
