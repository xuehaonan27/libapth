/*
 * test_sendto_recvfrom_apth.c
 *
 * Test sendto(2) and recvfrom(2) over a Unix-domain datagram socket pair
 * under libapth.
 *
 * Rationale for socketpair(AF_UNIX, SOCK_DGRAM)
 * ----------------------------------------------
 *   apth's hooked socket() is unimplemented (TODO → abort).
 *   socketpair(2) is NOT hooked and calls real libc.  AF_UNIX / SOCK_DGRAM
 *   gives us two connected datagram endpoints: each sendto() on one end
 *   delivers exactly one datagram to the other end's recvfrom().
 *
 * Scenario
 * --------
 *   • 1 sender apth sends N_MESSAGES datagrams via sendto() on fds[0].
 *     Each datagram is MSG_LEN bytes; byte 0 encodes the message sequence
 *     number (mod 256), all remaining bytes are 0xEF.
 *   • 1 receiver apth calls recvfrom() N_MESSAGES times on fds[1], verifying:
 *       – the returned length equals MSG_LEN
 *       – byte 0 matches the expected sequence number (mod 256)
 *       – all other bytes equal 0xEF
 *   • recvfrom() is called with a non-NULL src_addr / addrlen to exercise
 *     that code path as well.
 *
 * Pass criteria
 * -------------
 *   All N_MESSAGES datagrams are received intact, in order.
 *   Process exits 0 on pass, 1 on fail.
 */
#define _GNU_SOURCE
#include "apth.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ---- tunables ---- */
#define N_WORKERS    2
#define N_MESSAGES   256
#define MSG_LEN      128    /* must fit in SOCK_DGRAM socket buffer (≤ 128 KB) */

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
        /* Verify sequence byte */
        if ((unsigned char)buf[0] != (unsigned char)(i & 0xFF)) {
            fprintf(stderr,
                    "[FAIL] receiver: seq byte mismatch at msg %d "
                    "(got 0x%02x, expected 0x%02x)\n",
                    i, (unsigned char)buf[0], (unsigned char)(i & 0xFF));
            atomic_store(&g_pass, 0);
            return NULL;
        }
        /* Verify fill bytes */
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
APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, g_fds) != 0) {
        perror("socketpair(SOCK_DGRAM)");
        exit(1);
    }

    apth_t sender, receiver;
    apth_create(&sender,   NULL, sender_func,   NULL);
    apth_create(&receiver, NULL, receiver_func, NULL);

    apth_join(sender,   NULL);
    apth_join(receiver, NULL);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_sendto_recvfrom_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_sendto_recvfrom_apth\n");
    exit(1);
}
APTH_MAIN_END
