/*
 * test_fd_open_close_apth.c
 *
 * Test file-descriptor open/close lifecycle under libapth, with the same
 * fd shared across apths running on different schedulers.
 *
 * Scenario
 * --------
 *   • 4 worker threads (schedulers 0-3).
 *   • Main apth opens a temp file and stores the fd globally.
 *   • N_SEGMENTS writer apths, pinned round-robin to CPUs 0-3, each
 *     pwrite(2) a unique fill byte into their private slice of the file.
 *   • N_SEGMENTS reader apths, pinned round-robin to CPUs 0-3 (shifted
 *     by one so that opener and reader schedulers differ), each pread(2)
 *     their slice back and verify every byte.
 *   • A dedicated closer apth (CPU 3) calls the hooked close(2) on the
 *     shared fd, exercising apth_fd_unregister() and
 *     apth_notify_fd_closed() from a scheduler other than the opener's.
 *
 * Cross-scheduler stress
 * ----------------------
 *   open() is issued by the main apth (on whichever scheduler it lands on).
 *   pwrite/pread are called concurrently from apths on up to 4 schedulers,
 *   so apth_fd_acquire()/apth_fd_release() race on the shared APTH_FD_TABLE
 *   entry (testing the atomic refcount path).
 *   close() is called from a pinned apth on CPU 3, which is a different
 *   scheduler than the opener, exercising the cross-scheduler notify path.
 *
 * Pass criteria
 * -------------
 *   Every byte pread back matches the fill byte written by the corresponding
 *   pwrite.  Process exits 0 on pass, 1 on fail.
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

/* ---- tunables ---- */
#define N_WORKERS     4
#define N_SEGMENTS    8            /* concurrent writer / reader apths  */
#define SEGMENT_SIZE  4096         /* 4 KB per segment → 32 KB total    */
#define TOTAL_SIZE    ((size_t)(N_SEGMENTS) * (size_t)(SEGMENT_SIZE))
#define TMP_PATH      "/tmp/apth_test_fd_open_close.tmp"

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
    /* Each segment gets a unique fill byte so corruption is detectable. */
    unsigned char fill = (unsigned char)(0xA0 + (id & 0x07));
    memset(buf, fill, sizeof(buf));

    off_t off = (off_t)id * SEGMENT_SIZE;
    ssize_t rv = pwrite(g_fd, buf, SEGMENT_SIZE, off);
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

    off_t off = (off_t)id * SEGMENT_SIZE;
    ssize_t rv = pread(g_fd, buf, SEGMENT_SIZE, off);
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

/*
 * closer_func: calls close(g_fd) from a pinned scheduler (CPU 3).
 * This is intentionally a different CPU from the one that called open(),
 * so that apth_fd_unregister() and apth_notify_fd_closed() are issued
 * from a foreign scheduler.
 */
static void *closer_func(void *arg)
{
    (void)arg;
    int rc = close(g_fd);
    if (rc != 0) {
        fprintf(stderr, "[FAIL] closer: close() failed, errno=%d\n", errno);
        atomic_store(&g_pass, 0);
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

    /* Clean slate in case a previous run left the file behind. */
    unlink(TMP_PATH);

    /* Open the shared file from the main apth's scheduler. */
    g_fd = open(TMP_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (g_fd < 0) {
        perror("open");
        exit(1);
    }

    /* Pre-extend the file so every segment offset is valid. */
    if (ftruncate(g_fd, (off_t)TOTAL_SIZE) != 0) {
        perror("ftruncate");
        exit(1);
    }

    struct seg_arg args[N_SEGMENTS];
    apth_t writers[N_SEGMENTS];
    apth_t readers[N_SEGMENTS];

    /* -- Phase 1: N_SEGMENTS concurrent pwrite calls from N schedulers -- */
    for (int i = 0; i < N_SEGMENTS; i++) {
        args[i].id = i;
        apth_attr_t attr;
        make_pinned_attr(&attr, i);         /* CPUs: 0,1,2,3,0,1,2,3 */
        apth_create(&writers[i], &attr, writer_func, &args[i]);
        apth_attr_destroy(&attr);
    }
    for (int i = 0; i < N_SEGMENTS; i++)
        apth_join(writers[i], NULL);

    /* -- Phase 2: N_SEGMENTS concurrent pread calls from N schedulers -- */
    for (int i = 0; i < N_SEGMENTS; i++) {
        apth_attr_t attr;
        make_pinned_attr(&attr, i + 1);     /* shifted: 1,2,3,0,1,2,3,0 */
        apth_create(&readers[i], &attr, reader_func, &args[i]);
        apth_attr_destroy(&attr);
    }
    for (int i = 0; i < N_SEGMENTS; i++)
        apth_join(readers[i], NULL);

    /* -- Phase 3: close the shared fd from a different scheduler (CPU 3) -- */
    apth_t closer;
    {
        apth_attr_t attr;
        make_pinned_attr(&attr, 3);
        apth_create(&closer, &attr, closer_func, NULL);
        apth_attr_destroy(&attr);
    }
    apth_join(closer, NULL);

    /* Cleanup temp file. */
    unlink(TMP_PATH);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_fd_open_close_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_fd_open_close_apth\n");
    exit(1);
}
APTH_MAIN_END
