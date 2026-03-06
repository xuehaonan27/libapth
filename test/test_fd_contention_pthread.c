/*
 * test_fd_contention_pthread.c
 *
 * Pthread baseline for test_fd_contention_apth.c.
 * Same two-workload scenario – concurrent file open/close cycling alongside
 * shared-pipe I/O – using real pthreads and the unhooked libc calls.
 *
 * Pass criteria
 * -------------
 *   Every file-cycling read-back matches the written pattern.
 *   Total pipe bytes read == total pipe bytes written.
 *   Process exits 0 on pass, 1 on fail.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif // _GNU_SOURCE
#include <pthread.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>
#include <errno.h>

/* ---- tunables ---- */
#define N_CYCLERS        16
#define CYCLES           50
#define FILE_CHUNK       1024
#define N_PIPE_WRITERS   4
#define N_PIPE_READERS   4
#define PIPE_WRITE_CHUNK 128
#define PIPE_BYTES_PER_WRITER ((size_t)(4 * 1024))
#define PIPE_TOTAL       ((size_t)(N_PIPE_WRITERS) * PIPE_BYTES_PER_WRITER)

/* ---- globals ---- */
static int            g_pipefd[2];
static _Atomic size_t g_pipe_total_read = 0;
static _Atomic int    g_pass            = 1;

/* ------------------------------------------------------------------ */
struct cycler_arg { int id; };

static void *cycler_func(void *arg)
{
    struct cycler_arg *ca = (struct cycler_arg *)arg;
    int id = ca->id;

    char path[64];
    snprintf(path, sizeof(path), "/tmp/apth_fd_cnt_pt_%d.tmp", id);

    char wbuf[FILE_CHUNK];
    char rbuf[FILE_CHUNK];
    unsigned char fill = (unsigned char)(0xB0 + (id & 0x0F));
    memset(wbuf, fill, sizeof(wbuf));

    for (int c = 0; c < CYCLES; c++) {
        int wfd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (wfd < 0) {
            fprintf(stderr,
                    "[FAIL] cycler %d cycle %d: open(W) failed, errno=%d\n",
                    id, c, errno);
            atomic_store(&g_pass, 0);
            return NULL;
        }

        ssize_t ws = write(wfd, wbuf, FILE_CHUNK);
        if (ws != FILE_CHUNK) {
            fprintf(stderr,
                    "[FAIL] cycler %d cycle %d: write returned %zd\n",
                    id, c, ws);
            atomic_store(&g_pass, 0);
            close(wfd);
            return NULL;
        }
        close(wfd);

        int rfd = open(path, O_RDONLY);
        if (rfd < 0) {
            fprintf(stderr,
                    "[FAIL] cycler %d cycle %d: open(R) failed, errno=%d\n",
                    id, c, errno);
            atomic_store(&g_pass, 0);
            return NULL;
        }

        ssize_t rs = read(rfd, rbuf, FILE_CHUNK);
        if (rs != FILE_CHUNK) {
            fprintf(stderr,
                    "[FAIL] cycler %d cycle %d: read returned %zd\n",
                    id, c, rs);
            atomic_store(&g_pass, 0);
            close(rfd);
            return NULL;
        }

        for (int i = 0; i < FILE_CHUNK; i++) {
            if ((unsigned char)rbuf[i] != fill) {
                fprintf(stderr,
                        "[FAIL] cycler %d cycle %d: corruption at byte %d "
                        "(got 0x%02x, want 0x%02x)\n",
                        id, c, i, (unsigned char)rbuf[i], fill);
                atomic_store(&g_pass, 0);
                close(rfd);
                return NULL;
            }
        }
        close(rfd);
    }

    unlink(path);
    return NULL;
}

/* ------------------------------------------------------------------ */
static void *pipe_writer_func(void *arg)
{
    (void)arg;
    char buf[PIPE_WRITE_CHUNK];
    memset(buf, 0xCC, sizeof(buf));

    size_t remaining = PIPE_BYTES_PER_WRITER;
    while (remaining > 0) {
        size_t chunk = remaining < PIPE_WRITE_CHUNK ? remaining : PIPE_WRITE_CHUNK;
        ssize_t rv = write(g_pipefd[1], buf, chunk);
        if (rv <= 0) {
            fprintf(stderr, "[FAIL] pipe_writer: write() returned %zd\n", rv);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        remaining -= (size_t)rv;
    }
    return NULL;
}

static void *pipe_reader_func(void *arg)
{
    (void)arg;
    char buf[PIPE_WRITE_CHUNK];

    for (;;) {
        ssize_t rv = read(g_pipefd[0], buf, PIPE_WRITE_CHUNK);
        if (rv == 0)
            break;
        if (rv < 0) {
            fprintf(stderr, "[FAIL] pipe_reader: read() returned %zd\n", rv);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        for (ssize_t i = 0; i < rv; i++) {
            if ((unsigned char)buf[i] != 0xCC) {
                fprintf(stderr, "[FAIL] pipe_reader: byte corruption\n");
                atomic_store(&g_pass, 0);
                return NULL;
            }
        }
        atomic_fetch_add(&g_pipe_total_read, (size_t)rv);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
int main(void)
{
    if (pipe(g_pipefd) != 0) { perror("pipe"); return 1; }

    for (int i = 0; i < N_CYCLERS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/apth_fd_cnt_pt_%d.tmp", i);
        unlink(path);
    }

    struct cycler_arg cycler_args[N_CYCLERS];
    pthread_t cyclers[N_CYCLERS];
    pthread_t pipe_writers[N_PIPE_WRITERS];
    pthread_t pipe_readers[N_PIPE_READERS];

    for (int i = 0; i < N_CYCLERS; i++) {
        cycler_args[i].id = i;
        pthread_create(&cyclers[i], NULL, cycler_func, &cycler_args[i]);
    }
    for (int i = 0; i < N_PIPE_READERS; i++)
        pthread_create(&pipe_readers[i], NULL, pipe_reader_func, NULL);
    for (int i = 0; i < N_PIPE_WRITERS; i++)
        pthread_create(&pipe_writers[i], NULL, pipe_writer_func, NULL);

    for (int i = 0; i < N_PIPE_WRITERS; i++)
        pthread_join(pipe_writers[i], NULL);
    close(g_pipefd[1]);

    for (int i = 0; i < N_PIPE_READERS; i++)
        pthread_join(pipe_readers[i], NULL);
    for (int i = 0; i < N_CYCLERS; i++)
        pthread_join(cyclers[i], NULL);

    close(g_pipefd[0]);

    size_t got = atomic_load(&g_pipe_total_read);
    int pass = atomic_load(&g_pass) && (got == PIPE_TOTAL);
    if (pass) {
        fprintf(stderr,
                "[PASS] test_fd_contention_pthread  "
                "(%d cyclers × %d cycles, pipe %zu bytes)\n",
                N_CYCLERS, CYCLES, got);
        return 0;
    }
    fprintf(stderr,
            "[FAIL] test_fd_contention_pthread  "
            "(pass_flag=%d, pipe got=%zu expected=%zu)\n",
            atomic_load(&g_pass), got, PIPE_TOTAL);
    return 1;
}
