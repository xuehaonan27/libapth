/*
 * test_fd_contention_apth.c
 *
 * High-contention file-descriptor open/close test under libapth.
 *
 * Scenario
 * --------
 *   Two simultaneous workloads run concurrently on 4 worker threads:
 *
 *   1. File-cycling workload (N_CYCLERS apths):
 *      Each cycler apth repeatedly opens its own private temp file, writes
 *      a pattern, closes the file, reopens it read-only, reads the data
 *      back and verifies, then closes again.  With N_CYCLERS cyclers all
 *      running in parallel, the kernel rapidly recycles fd numbers (the
 *      low-numbered slots like 3, 4, 5 … keep getting reused).  Every
 *      open/close goes through the hooked open()/close() which calls
 *      apth_fd_register() / apth_fd_unregister() + apth_notify_fd_closed()
 *      concurrently from multiple schedulers.
 *
 *   2. Pipe-sharing workload (N_PIPE_WRITERS + N_PIPE_READERS apths):
 *      While file cycling is in progress, a separate group of apths hammers
 *      a shared pipe.  Writers write small chunks; readers read and count
 *      bytes.  This keeps the pipe fd's APTH_FD_TABLE slot under concurrent
 *      apth_fd_acquire() / apth_fd_release() pressure from multiple
 *      schedulers, interleaved with the register/unregister calls from the
 *      file cyclers.
 *
 * What this specifically exercises
 * ---------------------------------
 *   • Concurrent apth_fd_register() + apth_fd_unregister() from different
 *     schedulers (fd number reuse races in APTH_FD_TABLE).
 *   • apth_notify_fd_closed() flooding all scheduler pending_fd_close queues
 *     at high rate (N_CYCLERS × CYCLES × 2 close events total).
 *   • Concurrent apth_fd_acquire() / apth_fd_release() on a shared pipe fd
 *     while file-cycling I/O also modifies other FD table slots.
 *   • Correctness of per-file write-then-read-back under concurrent churn.
 *
 * Pass criteria
 * -------------
 *   Every file-cycling read-back matches the written pattern.
 *   Total pipe bytes read == total pipe bytes written.
 *   Process exits 0 on pass, 1 on fail.
 */
#define _GNU_SOURCE
#include "apth.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>
#include <sched.h>
#include <errno.h>
#include <sys/syscall.h>

/* ---- tunables ---- */
#define N_WORKERS        4
#define N_CYCLERS        16           /* file open/close cycling apths   */
#define CYCLES           50           /* iterations per cycler           */
#define FILE_CHUNK       1024         /* bytes written/read per cycle    */
#define N_PIPE_WRITERS   4
#define N_PIPE_READERS   4
#define PIPE_WRITE_CHUNK 128
#define PIPE_BYTES_PER_WRITER ((size_t)(4 * 1024))  /* 4 KB per writer   */
#define PIPE_TOTAL       ((size_t)(N_PIPE_WRITERS) * PIPE_BYTES_PER_WRITER)

/* ---- globals ---- */
static int            g_pipefd[2];
static _Atomic size_t g_pipe_total_read = 0;
static _Atomic int    g_pass            = 1;

/* ------------------------------------------------------------------ */
/*
 * cycler_func: open → write → close → open(rdonly) → read+verify → close,
 * repeated CYCLES times on a private temp file.
 *
 * The path is unique per cycler (index carried in arg) so there is no
 * inter-cycler data race on the file contents.  The fd-number pressure
 * comes from many cyclers running concurrently: once a cycler closes its
 * fd, the kernel may hand the same number to another cycler's next open().
 */
struct cycler_arg { int id; };

static void *cycler_func(void *arg)
{
    struct cycler_arg *ca = (struct cycler_arg *)arg;
    int id = ca->id;

    char path[64];
    snprintf(path, sizeof(path), "/tmp/apth_fd_cnt_%d.tmp", id);

    char wbuf[FILE_CHUNK];
    char rbuf[FILE_CHUNK];
    unsigned char fill = (unsigned char)(0xB0 + (id & 0x0F));
    memset(wbuf, fill, sizeof(wbuf));

    for (int c = 0; c < CYCLES; c++) {
        /* -- write phase -- */
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
                    "[FAIL] cycler %d cycle %d: write returned %zd, "
                    "errno=%d\n",
                    id, c, ws, errno);
            atomic_store(&g_pass, 0);
            close(wfd);
            return NULL;
        }

        close(wfd);

        /* -- read-back phase -- */
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
                    "[FAIL] cycler %d cycle %d: read returned %zd, "
                    "errno=%d\n",
                    id, c, rs, errno);
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

    /* Clean up temp file. */
    unlink(path);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Pipe workload: N_PIPE_WRITERS writers, N_PIPE_READERS readers. */

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
            fprintf(stderr,
                    "[FAIL] pipe_writer: write() returned %zd, errno=%d\n",
                    rv, errno);
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
            fprintf(stderr,
                    "[FAIL] pipe_reader: read() returned %zd, errno=%d\n",
                    rv, errno);
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

/* ---- helper: build an apth_attr pinned to CPU (cpu % N_WORKERS) ---- */
static void make_pinned_attr(apth_attr_t *attr, int cpu)
{
    apth_attr_init(attr);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu % N_WORKERS, &cpuset);
    apth_attr_setaffinity_np(attr, sizeof(cpuset), &cpuset);
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

    /* Pre-clean any leftover temp files. */
    for (int i = 0; i < N_CYCLERS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/apth_fd_cnt_%d.tmp", i);
        unlink(path);
    }

    struct cycler_arg cycler_args[N_CYCLERS];
    apth_t cyclers[N_CYCLERS];
    apth_t pipe_writers[N_PIPE_WRITERS];
    apth_t pipe_readers[N_PIPE_READERS];

    /* Launch file-cycling apths across all CPUs. */
    for (int i = 0; i < N_CYCLERS; i++) {
        cycler_args[i].id = i;
        apth_attr_t attr;
        make_pinned_attr(&attr, i);
        apth_create(&cyclers[i], &attr, cycler_func, &cycler_args[i]);
        apth_attr_destroy(&attr);
    }

    /* Launch pipe reader apths. */
    for (int i = 0; i < N_PIPE_READERS; i++) {
        apth_attr_t attr;
        make_pinned_attr(&attr, i + 1);
        apth_create(&pipe_readers[i], &attr, pipe_reader_func, NULL);
        apth_attr_destroy(&attr);
    }

    /* Launch pipe writer apths. */
    for (int i = 0; i < N_PIPE_WRITERS; i++) {
        apth_attr_t attr;
        make_pinned_attr(&attr, i + 2);
        apth_create(&pipe_writers[i], &attr, pipe_writer_func, NULL);
        apth_attr_destroy(&attr);
    }

    /* Wait for all pipe writers, then close write-end → readers see EOF. */
    for (int i = 0; i < N_PIPE_WRITERS; i++)
        apth_join(pipe_writers[i], NULL);
    close(g_pipefd[1]);

    /* Wait for all pipe readers. */
    for (int i = 0; i < N_PIPE_READERS; i++)
        apth_join(pipe_readers[i], NULL);

    /* Wait for all file cyclers. */
    for (int i = 0; i < N_CYCLERS; i++)
        apth_join(cyclers[i], NULL);

    close(g_pipefd[0]);

    /* Final checks. */
    size_t got = atomic_load(&g_pipe_total_read);
    int pass = atomic_load(&g_pass) && (got == PIPE_TOTAL);
    if (pass) {
        fprintf(stderr,
                "[PASS] test_fd_contention_apth  "
                "(%d cyclers × %d cycles, pipe %zu bytes)\n",
                N_CYCLERS, CYCLES, got);
        exit(0);
    }
    fprintf(stderr,
            "[FAIL] test_fd_contention_apth  "
            "(pass_flag=%d, pipe got=%zu expected=%zu)\n",
            atomic_load(&g_pass), got, PIPE_TOTAL);
    exit(1);
}
APTH_MAIN_END
