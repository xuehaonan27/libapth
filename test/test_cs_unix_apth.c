/*
 * test_cs_unix_apth.c
 *
 * Combined client-server test using read/write/send/recv under libapth.
 *
 * Scenario
 * --------
 *   N_PAIRS pairs of (server, client) apths share a socketpair each.
 *
 *   Server apth (for pair i):
 *     • Calls recv() to read a request of REQ_LEN bytes.
 *     • Verifies every byte equals (i & 0xFF).
 *     • Calls write() to echo the same REQ_LEN bytes back.
 *     • Repeats N_ROUNDS times.
 *
 *   Client apth (for pair i):
 *     • Calls send() to send REQ_LEN bytes (all equal to (i & 0xFF)).
 *     • Calls read() to receive the echo and verifies it byte-by-byte.
 *     • Repeats N_ROUNDS times.
 *
 *   This deliberately mixes write()/send() on the sending side and
 *   read()/recv() on the receiving side to confirm interoperability.
 *
 * Pass criteria
 * -------------
 *   All N_PAIRS × N_ROUNDS request/response cycles complete without error
 *   or data corruption.  Process exits 0 on pass, 1 on fail.
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
#define N_WORKERS   1
#define N_PAIRS     8
#define REQ_LEN     512
#define N_ROUNDS    32

/* ---- globals ---- */
/* fds[i][0] is the server side, fds[i][1] is the client side */
static int g_fds[N_PAIRS][2];
static _Atomic int g_pass = 1;

/* ------------------------------------------------------------------ */
static void *server_func(void *arg)
{
    int idx = (int)(intptr_t)arg;
    unsigned char fill = (unsigned char)(idx & 0xFF);
    char buf[REQ_LEN];

    for (int r = 0; r < N_ROUNDS; r++) {
        /* Receive request using recv() */
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
        /* Verify request content */
        for (size_t i = 0; i < REQ_LEN; i++) {
            if ((unsigned char)buf[i] != fill) {
                fprintf(stderr,
                        "[FAIL] server[%d]: request corruption at round %d "
                        "offset %zu (got 0x%02x)\n",
                        idx, r, i, (unsigned char)buf[i]);
                atomic_store(&g_pass, 0);
                return NULL;
            }
        }
        /* Echo back using write() */
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
        /* Send request using send() */
        ssize_t sv = send(g_fds[idx][1], buf, REQ_LEN, 0);
        if (sv != REQ_LEN) {
            fprintf(stderr,
                    "[FAIL] client[%d]: send() returned %zd at round %d\n",
                    idx, sv, r);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        /* Receive echo using read() */
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
        /* Verify echo */
        for (size_t i = 0; i < REQ_LEN; i++) {
            if ((unsigned char)rbuf[i] != fill) {
                fprintf(stderr,
                        "[FAIL] client[%d]: echo corruption at round %d "
                        "offset %zu (got 0x%02x)\n",
                        idx, r, i, (unsigned char)rbuf[i]);
                atomic_store(&g_pass, 0);
                return NULL;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    for (int i = 0; i < N_PAIRS; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, g_fds[i]) != 0) {
            perror("socketpair");
            exit(1);
        }
    }

    apth_t servers[N_PAIRS], clients[N_PAIRS];
    for (int i = 0; i < N_PAIRS; i++) {
        apth_create(&servers[i], NULL, server_func, (void *)(intptr_t)i);
        apth_create(&clients[i], NULL, client_func, (void *)(intptr_t)i);
    }
    for (int i = 0; i < N_PAIRS; i++) {
        apth_join(servers[i], NULL);
        apth_join(clients[i], NULL);
    }

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_cs_unix_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_cs_unix_apth\n");
    exit(1);
}
APTH_MAIN_END
