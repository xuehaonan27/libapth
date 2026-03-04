/*
 * test_pread_pwrite64_pthread.c
 *
 * Pthread baseline for test_pread_pwrite64_apth.c.
 * Tests pread64() and pwrite64() with large file offsets.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/types.h>

#define TEST_FILE "/tmp/test_pread_pwrite64_pthread.dat"
#define CHUNK_SIZE 4096
#define N_CHUNKS 4
#define OFFSET_BASE ((off64_t)1024 * 1024 * 1024)  /* 1GB offset */

static _Atomic int g_pass = 1;

static void *writer_func(void *arg)
{
    int fd = *(int *)arg;
    char buf[CHUNK_SIZE];

    for (int i = 0; i < N_CHUNKS; i++) {
        memset(buf, 0xA0 + i, CHUNK_SIZE);
        off64_t offset = OFFSET_BASE + (off64_t)(i * CHUNK_SIZE);
        ssize_t rv = pwrite64(fd, buf, CHUNK_SIZE, offset);
        if (rv != CHUNK_SIZE) {
            fprintf(stderr, "[FAIL] pwrite64 returned %zd, expected %d\n",
                    rv, CHUNK_SIZE);
            atomic_store(&g_pass, 0);
            return NULL;
        }
    }
    return NULL;
}

static void *reader_func(void *arg)
{
    int fd = *(int *)arg;
    char buf[CHUNK_SIZE];

    /* Give writer time to write */
    usleep(100000);

    for (int i = 0; i < N_CHUNKS; i++) {
        off64_t offset = OFFSET_BASE + (off64_t)(i * CHUNK_SIZE);
        ssize_t rv = pread64(fd, buf, CHUNK_SIZE, offset);
        if (rv != CHUNK_SIZE) {
            fprintf(stderr, "[FAIL] pread64 returned %zd, expected %d\n",
                    rv, CHUNK_SIZE);
            atomic_store(&g_pass, 0);
            return NULL;
        }

        /* Verify data */
        unsigned char expected = 0xA0 + i;
        for (int j = 0; j < CHUNK_SIZE; j++) {
            if ((unsigned char)buf[j] != expected) {
                fprintf(stderr, "[FAIL] data mismatch at chunk %d, byte %d\n", i, j);
                atomic_store(&g_pass, 0);
                return NULL;
            }
        }
    }
    return NULL;
}

int main(void)
{
    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    pthread_t writer, reader;
    pthread_create(&writer, NULL, writer_func, &fd);
    pthread_create(&reader, NULL, reader_func, &fd);

    pthread_join(writer, NULL);
    pthread_join(reader, NULL);

    close(fd);
    unlink(TEST_FILE);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_pread_pwrite64_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_pread_pwrite64_pthread\n");
    return 1;
}
