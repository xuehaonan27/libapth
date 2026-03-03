/*
 * test_fd_open_close_pthread.c
 *
 * Pthread baseline for test_fd_open_close_apth.c.
 * Same scenario – a file fd opened once and then shared across multiple
 * pthreads for concurrent pwrite / pread – using real pthreads and the
 * unhooked libc calls.
 *
 * Pass criteria
 * -------------
 *   Every byte pread back matches the fill byte written by the corresponding
 *   pwrite.  Process exits 0 on pass, 1 on fail.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>
#include <errno.h>

/* ---- tunables ---- */
#define N_SEGMENTS    8
#define SEGMENT_SIZE  4096
#define TOTAL_SIZE    ((size_t)(N_SEGMENTS) * (size_t)(SEGMENT_SIZE))
#define TMP_PATH      "/tmp/apth_test_fd_open_close_pt.tmp"

/* ---- globals ---- */
static int         g_fd;
static _Atomic int g_pass = 1;

/* ---- per-segment thread argument ---- */
struct seg_arg { int id; };

/* ------------------------------------------------------------------ */
static void *writer_func(void *arg)
{
    struct seg_arg *sa = (struct seg_arg *)arg;
    int id = sa->id;
    char buf[SEGMENT_SIZE];
    unsigned char fill = (unsigned char)(0xA0 + (id & 0x07));
    memset(buf, fill, sizeof(buf));

    ssize_t rv = pwrite(g_fd, buf, SEGMENT_SIZE, (off_t)id * SEGMENT_SIZE);
    if (rv != SEGMENT_SIZE) {
        fprintf(stderr,
                "[FAIL] writer %d: pwrite returned %zd (expected %d), "
                "errno=%d\n",
                id, rv, SEGMENT_SIZE, errno);
        atomic_store(&g_pass, 0);
    }
    return NULL;
}

static void *reader_func(void *arg)
{
    struct seg_arg *sa = (struct seg_arg *)arg;
    int id = sa->id;
    char buf[SEGMENT_SIZE];

    ssize_t rv = pread(g_fd, buf, SEGMENT_SIZE, (off_t)id * SEGMENT_SIZE);
    if (rv != SEGMENT_SIZE) {
        fprintf(stderr,
                "[FAIL] reader %d: pread returned %zd (expected %d), "
                "errno=%d\n",
                id, rv, SEGMENT_SIZE, errno);
        atomic_store(&g_pass, 0);
        return NULL;
    }

    unsigned char fill = (unsigned char)(0xA0 + (id & 0x07));
    for (int i = 0; i < SEGMENT_SIZE; i++) {
        if ((unsigned char)buf[i] != fill) {
            fprintf(stderr,
                    "[FAIL] reader %d: corruption at byte %d "
                    "(got 0x%02x, want 0x%02x)\n",
                    id, i, (unsigned char)buf[i], fill);
            atomic_store(&g_pass, 0);
            return NULL;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
int main(void)
{
    unlink(TMP_PATH);

    g_fd = open(TMP_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (g_fd < 0) { perror("open"); return 1; }

    if (ftruncate(g_fd, (off_t)TOTAL_SIZE) != 0) { perror("ftruncate"); return 1; }

    struct seg_arg args[N_SEGMENTS];
    pthread_t writers[N_SEGMENTS];
    pthread_t readers[N_SEGMENTS];

    /* Phase 1: concurrent pwrite */
    for (int i = 0; i < N_SEGMENTS; i++) {
        args[i].id = i;
        pthread_create(&writers[i], NULL, writer_func, &args[i]);
    }
    for (int i = 0; i < N_SEGMENTS; i++)
        pthread_join(writers[i], NULL);

    /* Phase 2: concurrent pread + verify */
    for (int i = 0; i < N_SEGMENTS; i++)
        pthread_create(&readers[i], NULL, reader_func, &args[i]);
    for (int i = 0; i < N_SEGMENTS; i++)
        pthread_join(readers[i], NULL);

    close(g_fd);
    unlink(TMP_PATH);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_fd_open_close_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_fd_open_close_pthread\n");
    return 1;
}
