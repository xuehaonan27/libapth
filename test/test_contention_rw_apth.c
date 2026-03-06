/*
 * test_contention_rw_apth.c
 *
 * High-contention read/write pipe test under libapth.
 *
 * Design goal
 * -----------
 *   Exercise the EAGAIN-retry logic and the fdmode race-condition fix under
 *   heavy concurrent I/O.  N_WRITERS apths and N_READERS apths share a
 *   single pipe: every writer competes for the write-end, every reader
 *   competes for the read-end.  With 4 worker pthreads, multiple apths are
 *   truly running in parallel on different CPUs.
 *
 * Scenario
 * --------
 *   • N_WRITERS writer apths each write BYTES_PER_WRITER bytes (all 0x5A)
 *     to the shared pipe write-end using write().
 *   • N_READERS reader apths all read from the shared read-end using read().
 *     Each reader accumulates bytes until it sees EOF.
 *   • After all writers join, the main apth closes the write-end via a raw
 *     Linux syscall so readers see EOF.
 *   • A shared atomic counter g_total_read tracks total bytes received.
 *
 * Pass criteria
 * -------------
 *   g_total_read == N_WRITERS × BYTES_PER_WRITER after all readers return.
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
#include <sys/syscall.h>

/* ---- tunables ---- */
#define N_WORKERS        4
#define N_WRITERS        20
#define N_READERS        20
#define BYTES_PER_WRITER ((size_t)(4 * 1024))   /* 4 KB each → 80 KB total */
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
            break;          /* EOF after write-end closed */
        if (rv < 0) {
            fprintf(stderr, "[FAIL] reader: read() returned %zd\n", rv);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        /* Verify fill byte */
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
APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    if (pipe(g_pipefd) != 0) {
        perror("pipe");
        exit(1);
    }

    apth_t writers[N_WRITERS], readers[N_READERS];
    for (int i = 0; i < N_WRITERS; i++)
        apth_create(&writers[i], NULL, writer_func, NULL);
    for (int i = 0; i < N_READERS; i++)
        apth_create(&readers[i], NULL, reader_func, NULL);

    /* Wait for all writers, then close write-end → readers see EOF */
    for (int i = 0; i < N_WRITERS; i++)
        apth_join(writers[i], NULL);
    syscall(SYS_close, g_pipefd[1]);

    for (int i = 0; i < N_READERS; i++)
        apth_join(readers[i], NULL);

    size_t got = atomic_load(&g_total_read);
    if (atomic_load(&g_pass) && got == TOTAL_BYTES) {
        fprintf(stderr,
                "[PASS] test_contention_rw_apth  (transferred %zu bytes)\n",
                got);
        exit(0);
    }
    fprintf(stderr,
            "[FAIL] test_contention_rw_apth  (got %zu, expected %zu)\n",
            got, TOTAL_BYTES);
    exit(1);
}
APTH_MAIN_END
