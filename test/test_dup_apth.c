/*
 * test_dup_apth.c
 *
 * Test dup(), dup2(), and dup3() file descriptor duplication under libapth.
 */
#define _GNU_SOURCE
#include "apth.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdatomic.h>

#define N_WORKERS 1
#define TEST_FILE "/tmp/test_dup_apth.dat"

static _Atomic int g_pass = 1;

static void *test_func(void *arg)
{
    (void)arg;
    int fd1, fd2, fd3, fd4;
    char buf[32];

    /* Create a test file */
    fd1 = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd1 < 0) {
        perror("open");
        atomic_store(&g_pass, 0);
        return NULL;
    }

    /* Write test data */
    const char *data = "Hello, dup test!";
    write(fd1, data, strlen(data));

    /* Test dup() */
    fd2 = dup(fd1);
    if (fd2 < 0) {
        fprintf(stderr, "[FAIL] dup() failed\n");
        atomic_store(&g_pass, 0);
        close(fd1);
        return NULL;
    }

    /* Read from duplicated fd */
    lseek(fd2, 0, SEEK_SET);
    ssize_t n = read(fd2, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    if (strcmp(buf, data) != 0) {
        fprintf(stderr, "[FAIL] dup() data mismatch\n");
        atomic_store(&g_pass, 0);
    }

    /* Test dup2() */
    fd3 = 100;  /* arbitrary fd number */
    if (dup2(fd1, fd3) < 0) {
        fprintf(stderr, "[FAIL] dup2() failed\n");
        atomic_store(&g_pass, 0);
        close(fd1);
        close(fd2);
        return NULL;
    }

    lseek(fd3, 0, SEEK_SET);
    n = read(fd3, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    if (strcmp(buf, data) != 0) {
        fprintf(stderr, "[FAIL] dup2() data mismatch\n");
        atomic_store(&g_pass, 0);
    }

    /* Test dup3() with O_CLOEXEC */
    fd4 = dup3(fd1, 101, O_CLOEXEC);
    if (fd4 < 0) {
        fprintf(stderr, "[FAIL] dup3() failed\n");
        atomic_store(&g_pass, 0);
        close(fd1);
        close(fd2);
        close(fd3);
        return NULL;
    }

    /* Verify O_CLOEXEC flag */
    int flags = fcntl(fd4, F_GETFD);
    if (!(flags & FD_CLOEXEC)) {
        fprintf(stderr, "[FAIL] dup3() O_CLOEXEC not set\n");
        atomic_store(&g_pass, 0);
    }

    close(fd1);
    close(fd2);
    close(fd3);
    close(fd4);
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    apth_t thread;
    apth_create(&thread, NULL, test_func, NULL);
    apth_join(thread, NULL);

    unlink(TEST_FILE);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_dup_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_dup_apth\n");
    exit(1);
}
APTH_MAIN_END
