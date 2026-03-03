/*
 * test_fd_shared_cross_sched_pthread.c
 *
 * Pthread baseline for test_fd_shared_cross_sched_apth.c.
 * Same scenario – a shared pipe, one writer, many readers, write-end closed
 * from the main thread – using real pthreads and the unhooked libc calls.
 *
 * Pass criteria
 * -------------
 *   Total bytes read across all readers == TOTAL_DATA.
 *   Every byte read equals FILL_BYTE.
 *   Process exits 0 on pass, 1 on fail.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>
#include <errno.h>

/* ---- tunables ---- */
#define N_READERS       12
#define WRITE_CHUNK     256
#define READ_CHUNK      512
#define TOTAL_DATA      ((size_t)(64 * 1024))
#define FILL_BYTE       0x7E

/* ---- globals ---- */
static int            g_pipefd[2];
static _Atomic size_t g_total_read = 0;
static _Atomic int    g_pass       = 1;

/* ------------------------------------------------------------------ */
static void *reader_func(void *arg)
{
    (void)arg;
    char buf[READ_CHUNK];

    for (;;) {
        ssize_t rv = read(g_pipefd[0], buf, READ_CHUNK);
        if (rv == 0)
            break;
        if (rv < 0) {
            fprintf(stderr, "[FAIL] reader: read() returned %zd, errno=%d\n",
                    rv, errno);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        for (ssize_t i = 0; i < rv; i++) {
            if ((unsigned char)buf[i] != FILL_BYTE) {
                fprintf(stderr,
                        "[FAIL] reader: byte corruption "
                        "(got 0x%02x, want 0x%02x)\n",
                        (unsigned char)buf[i], FILL_BYTE);
                atomic_store(&g_pass, 0);
                return NULL;
            }
        }
        atomic_fetch_add(&g_total_read, (size_t)rv);
    }
    return NULL;
}

static void *writer_func(void *arg)
{
    (void)arg;
    char buf[WRITE_CHUNK];
    memset(buf, FILL_BYTE, sizeof(buf));

    size_t remaining = TOTAL_DATA;
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

/* ------------------------------------------------------------------ */
int main(void)
{
    if (pipe(g_pipefd) != 0) { perror("pipe"); return 1; }

    pthread_t readers[N_READERS];
    for (int i = 0; i < N_READERS; i++)
        pthread_create(&readers[i], NULL, reader_func, NULL);

    pthread_t writer;
    pthread_create(&writer, NULL, writer_func, NULL);
    pthread_join(writer, NULL);

    /* Closing write-end makes readers see EOF. */
    close(g_pipefd[1]);

    for (int i = 0; i < N_READERS; i++)
        pthread_join(readers[i], NULL);

    close(g_pipefd[0]);

    size_t got = atomic_load(&g_total_read);
    if (atomic_load(&g_pass) && got == TOTAL_DATA) {
        fprintf(stderr,
                "[PASS] test_fd_shared_cross_sched_pthread  "
                "(%zu bytes, %d readers)\n",
                got, N_READERS);
        return 0;
    }
    fprintf(stderr,
            "[FAIL] test_fd_shared_cross_sched_pthread  "
            "(got %zu, expected %zu)\n",
            got, TOTAL_DATA);
    return 1;
}
