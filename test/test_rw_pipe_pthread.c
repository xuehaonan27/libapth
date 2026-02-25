/*
 * test_rw_pipe_pthread.c
 *
 * Pthread baseline for test_rw_pipe_apth.c.
 * Same scenario, using pthreads and the real (unhooked) libc calls.
 *
 * Pass criteria
 * -------------
 *   Reader receives exactly TOTAL_DATA bytes, all equal to 0xAB.
 *   Process exits 0 on pass, 1 on fail.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>

/* ---- tunables ---- */
#define CHUNK_SIZE 1024
#define N_CHUNKS   64
#define TOTAL_DATA ((size_t)(CHUNK_SIZE) * (size_t)(N_CHUNKS))   /* 64 KB */

/* ---- globals ---- */
static int g_pipefd[2];
static _Atomic int g_pass = 1;

/* ------------------------------------------------------------------ */
static void *writer_func(void *arg)
{
    (void)arg;
    char buf[CHUNK_SIZE];
    memset(buf, 0xAB, sizeof(buf));

    for (int i = 0; i < N_CHUNKS; i++) {
        ssize_t rv = write(g_pipefd[1], buf, CHUNK_SIZE);
        if (rv != CHUNK_SIZE) {
            fprintf(stderr,
                    "[FAIL] writer: write() returned %zd, expected %d\n",
                    rv, CHUNK_SIZE);
            atomic_store(&g_pass, 0);
            return NULL;
        }
    }
    return NULL;
}

static void *reader_func(void *arg)
{
    (void)arg;
    char buf[CHUNK_SIZE];
    size_t total = 0;

    for (;;) {
        ssize_t rv = read(g_pipefd[0], buf, sizeof(buf));
        if (rv == 0)
            break;          /* EOF */
        if (rv < 0) {
            fprintf(stderr, "[FAIL] reader: read() returned %zd\n", rv);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        for (ssize_t i = 0; i < rv; i++) {
            if ((unsigned char)buf[i] != 0xAB) {
                fprintf(stderr,
                        "[FAIL] reader: corruption at byte %zu (got 0x%02x)\n",
                        total + (size_t)i, (unsigned char)buf[i]);
                atomic_store(&g_pass, 0);
                return NULL;
            }
        }
        total += (size_t)rv;
    }

    if (total != TOTAL_DATA) {
        fprintf(stderr,
                "[FAIL] reader: got %zu bytes, expected %zu\n",
                total, TOTAL_DATA);
        atomic_store(&g_pass, 0);
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
    close(g_pipefd[1]);         /* close write-end → reader sees EOF */

    pthread_join(reader, NULL);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_rw_pipe_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_rw_pipe_pthread\n");
    return 1;
}
