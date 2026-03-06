/*
 * test_fork_wait_apth.c
 *
 * Test fork(), wait(), and waitpid() under libapth.
 * Note: fork() in userspace threading is complex. After fork, only the
 * calling thread exists in the child, and the APTH scheduler is stopped.
 * The child should use syscalls directly or exec immediately.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif // _GNU_SOURCE
#include "apth.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#define N_WORKERS 2

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
        /* Child process - use raw syscall to exit */
        syscall(SYS_exit, 42);
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
        fprintf(stderr, "[FAIL] child exit status incorrect: status=%d, exited=%d, exitstatus=%d\n",
                status, WIFEXITED(status), WEXITSTATUS(status));
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
        /* Child process - use raw syscall */
        syscall(SYS_nanosleep, &(struct timespec){.tv_sec = 0, .tv_nsec = 50000000}, NULL);
        syscall(SYS_exit, 99);
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
        fprintf(stderr, "[FAIL] child exit status incorrect in waitpid test: status=%d, exited=%d, exitstatus=%d\n",
                status, WIFEXITED(status), WEXITSTATUS(status));
        atomic_store(&g_pass, 0);
        return NULL;
    }

    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    apth_t t1, t2;
    apth_create(&t1, NULL, fork_test_func, NULL);
    apth_create(&t2, NULL, waitpid_test_func, NULL);

    apth_join(t1, NULL);
    apth_join(t2, NULL);

    if (atomic_load(&g_pass)) {
        fprintf(stderr, "[PASS] test_fork_wait_apth\n");
        exit(0);
    }
    fprintf(stderr, "[FAIL] test_fork_wait_apth\n");
    exit(1);
}
APTH_MAIN_END
