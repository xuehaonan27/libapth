/*
 * test_rw_pipe_apth.c
 *
 * Test read(2) and write(2) via a pipe under libapth.
 *
 * Scenario
 * --------
 *   • 1 writer apth writes TOTAL_DATA bytes (all 0xAB) in CHUNK_SIZE chunks
 *     using the hooked write().
 *   • 1 reader apth reads until EOF, verifying every byte is 0xAB.
 *   • After the writer joins, the main apth closes the write-end of the pipe
 *     via a direct Linux syscall (bypassing the apth-hooked close() which is
 *     currently unimplemented / TODO).
 *
 * Pass criteria
 * -------------
 *   Reader receives exactly TOTAL_DATA bytes, all equal to 0xAB.
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
#define N_WORKERS  2
#define CHUNK_SIZE 1024
#define N_CHUNKS   64
#define TOTAL_DATA ((size_t)(CHUNK_SIZE) * (size_t)(N_CHUNKS))   /* 64 KB */

/* ---- globals ---- */
static int g_pipefd[2];
static _Atomic int g_pass = 1;   /* set to 0 on first failure */

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
            break;          /* EOF — write-end was closed */
        if (rv < 0) {
            fprintf(stderr, "[FAIL] reader: read() returned %zd\n", rv);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        /* verify every byte */
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

    /* Let writer finish, then close write-end so reader sees EOF */
    apth_join(writer, NULL);
    syscall(SYS_close, g_pipefd[1]);    /* raw syscall: bypasses apth hook */

    apth_join(reader, NULL);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_rw_pipe_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_rw_pipe_apth\n");
    exit(1);
}
APTH_MAIN_END
