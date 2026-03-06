/*
 * test_contention_rw_pthread.c
 *
 * Pthread baseline for test_contention_rw_apth.c.
 * Same high-contention read/write pipe scenario using pthreads.
 *
 * Pass criteria
 * -------------
 *   g_total_read == N_WRITERS × BYTES_PER_WRITER.
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

/* ---- tunables ---- */
#define N_WRITERS        20
#define N_READERS        20
#define BYTES_PER_WRITER ((size_t)(4 * 1024))
#define WRITE_CHUNK      256
#define READ_CHUNK       512
#define TOTAL_BYTES      ((size_t)(N_WRITERS) * BYTES_PER_WRITER)

/* ---- globals ---- */
static int g_pipefd[2];
static _Atomic size_t g_total_read = 0;
static _Atomic int    g_pass       = 1;

/* ------------------------------------------------------------------ */
static void *writer_func(void *arg)
{
    (void)arg;
    char buf[WRITE_CHUNK];
    memset(buf, 0x5A, sizeof(buf));

    size_t remaining = BYTES_PER_WRITER;
    while (remaining > 0) {
        size_t chunk = remaining < WRITE_CHUNK ? remaining : WRITE_CHUNK;
        ssize_t rv = write(g_pipefd[1], buf, chunk);
        if (rv <= 0) {
            fprintf(stderr, "[FAIL] writer: write() returned %zd\n", rv);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        remaining -= (size_t)rv;
    }
    return NULL;
}

static void *reader_func(void *arg)
{
    (void)arg;
    char buf[READ_CHUNK];

    for (;;) {
        ssize_t rv = read(g_pipefd[0], buf, READ_CHUNK);
        if (rv == 0)
            break;
        if (rv < 0) {
            fprintf(stderr, "[FAIL] reader: read() returned %zd\n", rv);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        for (ssize_t i = 0; i < rv; i++) {
            if ((unsigned char)buf[i] != 0x5A) {
                fprintf(stderr, "[FAIL] reader: byte corruption\n");
                atomic_store(&g_pass, 0);
                return NULL;
            }
        }
        atomic_fetch_add(&g_total_read, (size_t)rv);
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

    pthread_t writers[N_WRITERS], readers[N_READERS];
    for (int i = 0; i < N_WRITERS; i++)
        pthread_create(&writers[i], NULL, writer_func, NULL);
    for (int i = 0; i < N_READERS; i++)
        pthread_create(&readers[i], NULL, reader_func, NULL);

    for (int i = 0; i < N_WRITERS; i++)
        pthread_join(writers[i], NULL);
    close(g_pipefd[1]);

    for (int i = 0; i < N_READERS; i++)
        pthread_join(readers[i], NULL);

    size_t got = atomic_load(&g_total_read);
    if (atomic_load(&g_pass) && got == TOTAL_BYTES) {
        fprintf(stderr,
                "[PASS] test_contention_rw_pthread  (transferred %zu bytes)\n",
                got);
        return 0;
    }
    fprintf(stderr,
            "[FAIL] test_contention_rw_pthread  (got %zu, expected %zu)\n",
            got, TOTAL_BYTES);
    return 1;
}
