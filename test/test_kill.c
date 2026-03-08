#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include "apth.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#define handle_error_en(en, msg) \
    do                           \
    {                            \
        errno = en;              \
        perror(msg);             \
        exit(EXIT_FAILURE);      \
    } while (0)

#define CHILD_RESULT ((void *)0xCAFEBABE)

static void *
thread_func(void *ignored_argument)
{
    (void)ignored_argument; // make compiler happy

    sigset_t sigs;
    int waited_sig;
    sigaddset(&sigs, SIGUSR1);
    sigwait(&sigs, &waited_sig);

    if (waited_sig != SIGUSR1)
    {
        static char err_msg[] = "child do not get SIGUSR1\n";
        if (write(2, err_msg, sizeof(err_msg)) < 0) {
            fprintf(stderr, "[FAIL] write() failed\n");
            return NULL;
        }
    }
    else
    {
        static char child_msg[] = "child got the signal SIGUSR1\n";
        if (write(2, child_msg, sizeof(child_msg)) < 0) {
            fprintf(stderr, "[FAIL] write() failed\n");
            return NULL;
        }
    }
    apth_exit(CHILD_RESULT);

    // Make compiler happy
    perror("Should not reach here");
    return (void *)(intptr_t)(-1);
}

APTH_CONFIG(cfg,
            cfg->workers = 1;)

APTH_MAIN_BEGIN(argc, argv)
{
    apth_t thr;
    int s;

    s = apth_create(&thr, NULL, &thread_func, NULL);
    if (s != 0)
        handle_error_en(s, "apth_create");

    sleep(2); /* Give thread a chance to get started */

    // Main thread kill a signal to child thread
    apth_kill(thr, SIGUSR1);

    static char main_msg[] = "main sent signal SIGUSR1\n";
    if (write(2, main_msg, sizeof(main_msg)) < 0) {
        fprintf(stderr, "[FAIL] write() failed\n");
        return NULL;
    }

    void *cdata;
    apth_join(thr, &cdata);

    if (cdata != CHILD_RESULT)
    {
        static char main_err_msg[] = "main got wrong child result\n";
        if (write(2, main_err_msg, sizeof(main_err_msg)) < 0) {
            fprintf(stderr, "[FAIL] write() failed\n");
            return NULL;
        }
    }
    else
    {
        static char main_join_msg[] = "main joined the child\n";
        if (write(2, main_join_msg, sizeof(main_join_msg)) < 0) {
            fprintf(stderr, "[FAIL] write() failed\n");
            return NULL;
        }
    }
    apth_exit(NULL);

    // Make compiler happy
    perror("Should not reach here");
    return (void *)(intptr_t)(-1);
}
APTH_MAIN_END