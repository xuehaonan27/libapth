#include "apth.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

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

    // NOTE: in the test case, we SHOULD see this message printed
    static char child_msg[] = "child: going to exit by myself\n";
    write(2, child_msg, sizeof(child_msg));
    apth_exit(NULL);
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

    // Main thread call apth_exit
    static char main_msg[] = "main: going to call apth_exit\n";
    write(2, main_msg, sizeof(main_msg));
    apth_exit(NULL);
}
APTH_MAIN_END
