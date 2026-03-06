/*
 * test_rwv_pipe_pthread.c
 *
 * Pthread baseline for test_rwv_pipe_apth.c.
 * Same scatter-gather scenario using pthreads and real libc calls.
 *
 * Pass criteria
 * -------------
 *   All N_ROUNDS readv() calls complete without data corruption.
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
#include <sys/uio.h>

/* ---- tunables ---- */
#define SEG0_LEN   256
#define SEG1_LEN   512
#define SEG2_LEN   256
#define RECORD_LEN (SEG0_LEN + SEG1_LEN + SEG2_LEN)  /* 1024 */
#define N_ROUNDS   64

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
    char accum[RECORD_LEN];
    size_t acc_off = 0;

    for (int r = 0; r < N_ROUNDS; ) {
        size_t need = RECORD_LEN - acc_off;
        char *p = accum + acc_off;
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
            /* We requested exactly RECORD_LEN - acc_off bytes, so after
             * reading rv bytes, acc_off == RECORD_LEN.  Reset to 0. */
            acc_off = 0;
            r++;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
int main(void)
{
    if (pipe(g_pipefd) != 0) {
        perror("pipe");
        return 1;
    }

    pthread_t writer, reader;
    pthread_create(&writer, NULL, writer_func, NULL);
    pthread_create(&reader, NULL, reader_func, NULL);

    pthread_join(writer, NULL);
    close(g_pipefd[1]);

    pthread_join(reader, NULL);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_rwv_pipe_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_rwv_pipe_pthread\n");
    return 1;
}
