/*
 * test_fork_wait_pthread.c
 *
 * Pthread baseline for test_fork_wait_apth.c.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif // _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/wait.h>

static _Atomic int g_pass = 1;

static void *fork_test_func(void *arg)
{
    (void)arg;

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[FAIL] fork() failed\n");
        atomic_store(&g_pass, 0);
        return NULL;
    }

    if (pid == 0) {
        /* Child process */
        exit(42);  /* Exit with specific code */
    }

    /* Parent process */
    int status;
    pid_t waited = wait(&status);
    if (waited != pid) {
        fprintf(stderr, "[FAIL] wait() returned wrong pid\n");
        atomic_store(&g_pass, 0);
        return NULL;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) {
        fprintf(stderr, "[FAIL] child exit status incorrect\n");
        atomic_store(&g_pass, 0);
        return NULL;
    }

    return NULL;
}

static void *waitpid_test_func(void *arg)
{
    (void)arg;

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[FAIL] fork() failed in waitpid test\n");
        atomic_store(&g_pass, 0);
        return NULL;
    }

    if (pid == 0) {
        /* Child process */
        usleep(50000);  /* Sleep a bit */
        exit(99);
    }

    /* Parent process - use waitpid */
    int status;
    pid_t waited = waitpid(pid, &status, 0);
    if (waited != pid) {
        fprintf(stderr, "[FAIL] waitpid() returned wrong pid\n");
        atomic_store(&g_pass, 0);
        return NULL;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 99) {
        fprintf(stderr, "[FAIL] child exit status incorrect in waitpid test\n");
        atomic_store(&g_pass, 0);
        return NULL;
    }

    return NULL;
}

int main(void)
{
    pthread_t t1, t2;
    pthread_create(&t1, NULL, fork_test_func, NULL);
    pthread_create(&t2, NULL, waitpid_test_func, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_fork_wait_pthread\n");
        return 0;
    }
    fprintf(stderr, "[FAIL] test_fork_wait_pthread\n");
    return 1;
}
