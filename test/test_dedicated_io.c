/*
 * test_dedicated_io.c -- Test I/O between dedicated and regular threads
 *
 * Verify pipe-based I/O works in both directions:
 *   1. Dedicated thread writes "hello" to pipe, regular apth reads it.
 *   2. Regular apth writes "world" to pipe, dedicated thread reads it.
 *
 * Dedicated threads bypass hooked I/O (raw blocking syscalls), while
 * regular apths use hooked non-blocking I/O.  Both directions must work.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "apth.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/syscall.h>

#define N_WORKERS 2

/* Pipe for direction 1: dedicated writes, regular reads */
static int g_pipe1[2];
/* Pipe for direction 2: regular writes, dedicated reads */
static int g_pipe2[2];

static _Atomic(int) g_pass = 1;

/* --- Direction 1: dedicated writer --- */
static void *
dedicated_writer(void *arg)
{
    (void)arg;
    const char msg[] = "hello";
    ssize_t nw = write(g_pipe1[1], msg, 5);
    if (nw != 5)
    {
        static char err[] = "test_dedicated_io: dedicated write failed\n";
        write(2, err, sizeof(err) - 1);
        atomic_store(&g_pass, 0);
    }
    return NULL;
}

/* --- Direction 1: regular reader --- */
static void *
regular_reader(void *arg)
{
    (void)arg;
    char buf[16];
    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(g_pipe1[0], buf, sizeof(buf));
    if (nr != 5 || memcmp(buf, "hello", 5) != 0)
    {
        static char err[] = "test_dedicated_io: regular read mismatch\n";
        write(2, err, sizeof(err) - 1);
        atomic_store(&g_pass, 0);
    }
    return NULL;
}

/* --- Direction 2: regular writer --- */
static void *
regular_writer(void *arg)
{
    (void)arg;
    const char msg[] = "world";
    ssize_t nw = write(g_pipe2[1], msg, 5);
    if (nw != 5)
    {
        static char err[] = "test_dedicated_io: regular write failed\n";
        write(2, err, sizeof(err) - 1);
        atomic_store(&g_pass, 0);
    }
    return NULL;
}

/* --- Direction 2: dedicated reader --- */
static void *
dedicated_reader(void *arg)
{
    (void)arg;
    char buf[16];
    memset(buf, 0, sizeof(buf));
    ssize_t nr = read(g_pipe2[0], buf, sizeof(buf));
    if (nr != 5 || memcmp(buf, "world", 5) != 0)
    {
        static char err[] = "test_dedicated_io: dedicated read mismatch\n";
        write(2, err, sizeof(err) - 1);
        atomic_store(&g_pass, 0);
    }
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char starting[] = "test_dedicated_io: starting\n";
    write(2, starting, sizeof(starting) - 1);

    /* Create pipes */
    if (pipe(g_pipe1) != 0 || pipe(g_pipe2) != 0)
    {
        static char err[] = "test_dedicated_io: FAIL (pipe creation failed)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Direction 1: dedicated writes, regular reads */
    {
        apth_t wr_th, rd_th;

        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
        int rc = apth_create(&wr_th, &attr, dedicated_writer, NULL);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] = "test_dedicated_io: FAIL (dedicated_writer create)\n";
            write(2, err, sizeof(err) - 1);
            exit(1);
        }

        rc = apth_create(&rd_th, NULL, regular_reader, NULL);
        if (rc != 0)
        {
            static char err[] = "test_dedicated_io: FAIL (regular_reader create)\n";
            write(2, err, sizeof(err) - 1);
            exit(1);
        }

        apth_join(wr_th, NULL);
        /* Close write end so reader sees data (not EOF -- data already written) */
        syscall(SYS_close, g_pipe1[1]);
        apth_join(rd_th, NULL);
        syscall(SYS_close, g_pipe1[0]);
    }

    /* Direction 2: regular writes, dedicated reads */
    {
        apth_t wr_th, rd_th;

        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
        int rc = apth_create(&rd_th, &attr, dedicated_reader, NULL);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] = "test_dedicated_io: FAIL (dedicated_reader create)\n";
            write(2, err, sizeof(err) - 1);
            exit(1);
        }

        rc = apth_create(&wr_th, NULL, regular_writer, NULL);
        if (rc != 0)
        {
            static char err[] = "test_dedicated_io: FAIL (regular_writer create)\n";
            write(2, err, sizeof(err) - 1);
            exit(1);
        }

        apth_join(wr_th, NULL);
        /* Close write end so dedicated reader can finish */
        syscall(SYS_close, g_pipe2[1]);
        apth_join(rd_th, NULL);
        syscall(SYS_close, g_pipe2[0]);
    }

    if (atomic_load(&g_pass))
    {
        static char ok[] = "test_dedicated_io: PASS\n";
        write(2, ok, sizeof(ok) - 1);
        exit(0);
    }

    static char fail[] = "test_dedicated_io: FAIL\n";
    write(2, fail, sizeof(fail) - 1);
    exit(1);
}
APTH_MAIN_END
