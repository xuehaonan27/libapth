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

static void *
thread_func(void *ignored_argument)
{
    (void)ignored_argument; // make compiler happy

    /* sleep() is a cancelation point. */
    sleep(5);

    // NOTE: in the test case, we SHOULD NOT see this message printed
    static char child_msg[] = "child: going to exit by myself\n";
    if (write(2, child_msg, sizeof(child_msg)) < 0) {
        fprintf(stderr, "[FAIL] write() failed\n");
        return NULL;
    }
    apth_exit(NULL);

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

    // Main thread returns
    static char main_msg[] = "main: going to return\n";
    if (write(2, main_msg, sizeof(main_msg)) < 0) {
        fprintf(stderr, "[FAIL] write() failed\n");
        return NULL;
    }
}
APTH_MAIN_END
