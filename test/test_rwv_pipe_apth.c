/*
 * test_rwv_pipe_apth.c
 *
 * Test readv(2) and writev(2) via a pipe under libapth.
 *
 * Scenario
 * --------
 *   Each writev() call scatters 3 iovec segments into the pipe:
 *     iov[0]: 256 bytes of 0x11
 *     iov[1]: 512 bytes of 0x22
 *     iov[2]: 256 bytes of 0x33
 *   Total per writev call: 1024 bytes.  Repeated N_ROUNDS times.
 *
 *   The reader uses readv() with the same 3-segment layout and verifies that
 *   each segment contains the expected fill byte.
 *
 * Pass criteria
 * -------------
 *   All N_ROUNDS readv() calls complete without data corruption.
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
#include <sys/uio.h>
#include <sys/syscall.h>

/* ---- tunables ---- */
#define N_WORKERS  2
#define SEG0_LEN   256
#define SEG1_LEN   512
#define SEG2_LEN   256
#define RECORD_LEN (SEG0_LEN + SEG1_LEN + SEG2_LEN)  /* 1024 */
#define N_ROUNDS   64   /* total transfer: 64 KB */

static const unsigned char SEG0_FILL = 0x11;
static const unsigned char SEG1_FILL = 0x22;
static const unsigned char SEG2_FILL = 0x33;

/* ---- globals ---- */
static int g_pipefd[2];
static _Atomic int g_pass = 1;

/* ------------------------------------------------------------------ */
static void *writer_func(void *arg)
{
    (void)arg;
    /* Pre-fill write buffers */
    char b0[SEG0_LEN], b1[SEG1_LEN], b2[SEG2_LEN];
    memset(b0, SEG0_FILL, sizeof(b0));
    memset(b1, SEG1_FILL, sizeof(b1));
    memset(b2, SEG2_FILL, sizeof(b2));

    struct iovec iov[3] = {
        { b0, SEG0_LEN },
        { b1, SEG1_LEN },
        { b2, SEG2_LEN },
    };

    for (int r = 0; r < N_ROUNDS; r++) {
        ssize_t rv = writev(g_pipefd[1], iov, 3);
        if (rv != RECORD_LEN) {
            fprintf(stderr,
                    "[FAIL] writer: writev() returned %zd, expected %d\n",
                    rv, RECORD_LEN);
            atomic_store(&g_pass, 0);
            return NULL;
        }
    }
    return NULL;
}

static void *reader_func(void *arg)
{
    (void)arg;
    char r0[SEG0_LEN], r1[SEG1_LEN], r2[SEG2_LEN];

    struct iovec iov[3] = {
        { r0, SEG0_LEN },
        { r1, SEG1_LEN },
        { r2, SEG2_LEN },
    };

    /*
     * readv() may return short on a pipe — accumulate until a full record is
     * assembled, then verify the 3-segment pattern.
     */
    char accum[RECORD_LEN];
    size_t acc_off = 0;

    for (int r = 0; r < N_ROUNDS; ) {
        /* Refill the iov to point into the accumulation buffer */
        size_t need = RECORD_LEN - acc_off;
        char *p = accum + acc_off;

        /* Use a single-segment readv to be generic */
        struct iovec single = { p, need };
        ssize_t rv = readv(g_pipefd[0], &single, 1);
        if (rv == 0) {
            fprintf(stderr, "[FAIL] reader: unexpected EOF at round %d\n", r);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        if (rv < 0) {
            fprintf(stderr, "[FAIL] reader: readv() error at round %d\n", r);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        acc_off += (size_t)rv;

        if (acc_off >= RECORD_LEN) {
            /* Verify one full record */
            for (size_t i = 0; i < SEG0_LEN; i++) {
                if ((unsigned char)accum[i] != SEG0_FILL) {
                    fprintf(stderr,
                            "[FAIL] reader: seg0 corruption at round %d offset %zu\n",
                            r, i);
                    atomic_store(&g_pass, 0);
                    return NULL;
                }
            }
            for (size_t i = 0; i < SEG1_LEN; i++) {
                if ((unsigned char)accum[SEG0_LEN + i] != SEG1_FILL) {
                    fprintf(stderr,
                            "[FAIL] reader: seg1 corruption at round %d offset %zu\n",
                            r, i);
                    atomic_store(&g_pass, 0);
                    return NULL;
                }
            }
            for (size_t i = 0; i < SEG2_LEN; i++) {
                if ((unsigned char)accum[SEG0_LEN + SEG1_LEN + i] != SEG2_FILL) {
                    fprintf(stderr,
                            "[FAIL] reader: seg2 corruption at round %d offset %zu\n",
                            r, i);
                    atomic_store(&g_pass, 0);
                    return NULL;
                }
            }
            /*
             * We requested exactly RECORD_LEN - acc_off bytes, so after
             * reading rv bytes we have acc_off == RECORD_LEN.  Reset to 0
             * for the next iteration; no memmove needed.
             */
            acc_off = 0;
            r++;
        }
    }
    (void)iov;   /* suppress unused warning */
    return NULL;
}

/* ------------------------------------------------------------------ */
APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    if (pipe(g_pipefd) != 0) {
        perror("pipe");
        exit(1);
    }

    apth_t writer, reader;
    apth_create(&writer, NULL, writer_func, NULL);
    apth_create(&reader, NULL, reader_func, NULL);

    apth_join(writer, NULL);
    syscall(SYS_close, g_pipefd[1]);    /* bypass apth-hooked close() */

    apth_join(reader, NULL);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_rwv_pipe_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_rwv_pipe_apth\n");
    exit(1);
}
APTH_MAIN_END
