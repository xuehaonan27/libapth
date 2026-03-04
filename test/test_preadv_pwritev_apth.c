/*
 * test_preadv_pwritev_apth.c
 *
 * Test preadv() and pwritev() scatter-gather I/O under libapth.
 */
#define _GNU_SOURCE
#include "apth.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/uio.h>

#define N_WORKERS 2
#define TEST_FILE "/tmp/test_preadv_pwritev_apth.dat"
#define N_IOVECS 3
#define IOV_SIZE 1024

static int g_fd;
static _Atomic int g_pass = 1;

static void *writer_func(void *arg)
{
    (void)arg;
    char buf1[IOV_SIZE], buf2[IOV_SIZE], buf3[IOV_SIZE];

    memset(buf1, 0xAA, IOV_SIZE);
    memset(buf2, 0xBB, IOV_SIZE);
    memset(buf3, 0xCC, IOV_SIZE);

    struct iovec iov[N_IOVECS];
    iov[0].iov_base = buf1;
    iov[0].iov_len = IOV_SIZE;
    iov[1].iov_base = buf2;
    iov[1].iov_len = IOV_SIZE;
    iov[2].iov_base = buf3;
    iov[2].iov_len = IOV_SIZE;

    ssize_t rv = pwritev(g_fd, iov, N_IOVECS, 0);
    if (rv != IOV_SIZE * N_IOVECS) {
        fprintf(stderr, "[FAIL] pwritev returned %zd, expected %d\n",
                rv, IOV_SIZE * N_IOVECS);
        atomic_store(&g_pass, 0);
    }
    return NULL;
}

static void *reader_func(void *arg)
{
    (void)arg;
    char buf1[IOV_SIZE], buf2[IOV_SIZE], buf3[IOV_SIZE];

    usleep(50000);

    struct iovec iov[N_IOVECS];
    iov[0].iov_base = buf1;
    iov[0].iov_len = IOV_SIZE;
    iov[1].iov_base = buf2;
    iov[1].iov_len = IOV_SIZE;
    iov[2].iov_base = buf3;
    iov[2].iov_len = IOV_SIZE;

    ssize_t rv = preadv(g_fd, iov, N_IOVECS, 0);
    if (rv != IOV_SIZE * N_IOVECS) {
        fprintf(stderr, "[FAIL] preadv returned %zd, expected %d\n",
                rv, IOV_SIZE * N_IOVECS);
        atomic_store(&g_pass, 0);
        return NULL;
    }

    /* Verify data */
    for (int i = 0; i < IOV_SIZE; i++) {
        if ((unsigned char)buf1[i] != 0xAA ||
            (unsigned char)buf2[i] != 0xBB ||
            (unsigned char)buf3[i] != 0xCC) {
            fprintf(stderr, "[FAIL] data mismatch at byte %d\n", i);
            atomic_store(&g_pass, 0);
            return NULL;
        }
    }
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    g_fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (g_fd < 0) {
        perror("open");
        exit(1);
    }

    apth_t writer, reader;
    apth_create(&writer, NULL, writer_func, NULL);
    apth_create(&reader, NULL, reader_func, NULL);

    apth_join(writer, NULL);
    apth_join(reader, NULL);

    close(g_fd);
    unlink(TEST_FILE);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_preadv_pwritev_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_preadv_pwritev_apth\n");
    exit(1);
}
APTH_MAIN_END
