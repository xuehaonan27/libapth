/*
 * test_fd_shared_cross_sched_apth.c
 *
 * Test cross-scheduler fd close notification under libapth.
 *
 * Scenario
 * --------
 *   • 4 worker threads (schedulers 0-3).
 *   • 1 pipe: pipefd[0] (read-end) / pipefd[1] (write-end).
 *   • N_READERS reader apths, pinned round-robin to CPUs 0-3, each reading
 *     from the shared pipefd[0] in a loop until EOF.  They accumulate the
 *     total bytes received and verify the fill byte.
 *   • 1 writer apth (pinned to CPU 0) writing TOTAL_DATA bytes of FILL_BYTE
 *     in WRITE_CHUNK-sized chunks to pipefd[1].
 *   • After the writer joins, the main apth calls the hooked close(pipefd[1]).
 *     This triggers:
 *       1. apth_fd_unregister(pipefd[1])  – removes write-end from FD table.
 *       2. apth_notify_fd_closed(pipefd[1]) – broadcasts close notification
 *          into every scheduler's pending_fd_close queue (cross-scheduler
 *          notification path).
 *       3. Kernel closes pipefd[1]; the pipe becomes EOF-readable on
 *          pipefd[0], so epoll on each scheduler fires EPOLLIN for readers
 *          that are blocked.
 *     Reader apths on schedulers 1, 2, and 3 (different from the closer's
 *     scheduler) unblock via their per-scheduler epoll instances and see
 *     rv == 0 (EOF), then return.
 *
 * What this specifically exercises
 * ---------------------------------
 *   • apth_notify_fd_closed() broadcasting to all scheduler pending queues.
 *   • apth_sched_process_pending_fd_closes() draining those queues.
 *   • Per-scheduler epoll waking readers on remote schedulers when the pipe
 *     write-end is closed from a different scheduler.
 *   • Correct total-byte accounting across many concurrent readers.
 *
 * Pass criteria
 * -------------
 *   Total bytes read across all readers == TOTAL_DATA.
 *   Every byte read equals FILL_BYTE.
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
#include <sched.h>
#include <errno.h>

/* ---- tunables ---- */
#define N_WORKERS       4
#define N_READERS       12            /* 3 readers per scheduler         */
#define WRITE_CHUNK     256
#define READ_CHUNK      512
#define TOTAL_DATA      ((size_t)(64 * 1024))  /* 64 KB total             */
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
            break;      /* EOF: write-end was closed */
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
            fprintf(stderr,
                    "[FAIL] writer: write() returned %zd, errno=%d\n",
                    rv, errno);
            atomic_store(&g_pass, 0);
            return NULL;
        }
        remaining -= (size_t)rv;
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

    /* Spawn N_READERS reader apths spread evenly across all 4 CPUs.
     * With N_READERS=12 and N_WORKERS=4 this places 3 readers per
     * scheduler, ensuring readers on schedulers 1, 2, and 3 will be
     * blocked in their per-scheduler epoll when the main apth closes
     * pipefd[1] from its own scheduler. */
    apth_t readers[N_READERS];
    for (int i = 0; i < N_READERS; i++) {
        apth_attr_t attr;
        make_pinned_attr(&attr, i);     /* CPUs: 0,1,2,3,0,1,2,3,0,1,2,3 */
        apth_create(&readers[i], &attr, reader_func, NULL);
        apth_attr_destroy(&attr);
    }

    /* Writer on CPU 0 — same scheduler as three of the readers; the other
     * nine readers live on schedulers 1, 2, and 3. */
    apth_t writer;
    {
        apth_attr_t attr;
        make_pinned_attr(&attr, 0);
        apth_create(&writer, &attr, writer_func, NULL);
        apth_attr_destroy(&attr);
    }

    /* Wait for all data to be written. */
    apth_join(writer, NULL);

    /*
     * Close the write-end of the pipe from the main apth's scheduler.
     *
     * The hooked close() calls:
     *   apth_fd_unregister(pipefd[1])      – clears APTH_FD_TABLE entry
     *   apth_notify_fd_closed(pipefd[1])   – pushes pipefd[1] into every
     *                                         scheduler's pending_fd_close queue
     *   apth_func_raw(close)(pipefd[1]) – actual kernel close
     *
     * After the kernel close the pipe's read-end becomes EOF-readable.
     * Each scheduler's event manager drains its pending_fd_close queue and
     * then its epoll instance fires EPOLLIN (EOF) on pipefd[0], waking
     * readers that are currently blocked across all schedulers.
     */
    close(g_pipefd[1]);

    /* All readers will see EOF and return. */
    for (int i = 0; i < N_READERS; i++)
        apth_join(readers[i], NULL);

    /* Close the read-end after all readers are done. */
    close(g_pipefd[0]);

    size_t got = atomic_load(&g_total_read);
    if (atomic_load(&g_pass) && got == TOTAL_DATA) {
        fprintf(stderr,
                "[PASS] test_fd_shared_cross_sched_apth  "
                "(%zu bytes across %d schedulers, %d readers)\n",
                got, N_WORKERS, N_READERS);
        exit(0);
    }
    fprintf(stderr,
            "[FAIL] test_fd_shared_cross_sched_apth  "
            "(got %zu, expected %zu, pass_flag=%d)\n",
            got, TOTAL_DATA, atomic_load(&g_pass));
    exit(1);
}
APTH_MAIN_END
