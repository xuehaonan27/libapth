/*
 * try_main_macro.c — same test as try_main_normal.c, rewritten with the
 * APTH_CONFIG / APTH_MAIN_BEGIN / APTH_MAIN_END convenience macros.
 *
 * Compared to try_main_normal.c the user no longer needs to:
 *   • define struct main_args
 *   • write void *apth_main(void *) with manual arg unpacking
 *   • write int main() with apth_init_t / apth_initvals_init / apth_init
 *
 * The only things the user writes are:
 *   1. (optional) APTH_CONFIG — to customise library init parameters
 *   2. APTH_MAIN_BEGIN / APTH_MAIN_END — the actual program logic
 */

#include "apth.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#define NULL ((void *)0)

#define handle_error_en(en, msg) \
    do                           \
    {                            \
        errno = en;              \
        perror(msg);             \
        exit(EXIT_FAILURE);      \
    } while (0)

/* ---- helper thread (unchanged from the normal version) ---- */

static void *
thread_func(void *ignored_argument)
{
    (void)ignored_argument; // make compiler happy

    int s;

    s = apth_setcancelstate(APTH_CANCEL_DISABLE, NULL);
    if (s != 0)
        handle_error_en(s, "apth_setcancelstate");

    // printf("%s(): started; cancelation disabled\n", __func__);
    static char child_msg_1[] = "child: started; cancelation disabled\n";
    write(2, child_msg_1, sizeof(child_msg_1));
    sleep(5);
    // printf("%s(): about to enable cancelation\n", __func__);
    static char child_msg_2[] = "child: about to enable cancelation\n";
    write(2, child_msg_2, sizeof(child_msg_2));

    s = apth_setcancelstate(APTH_CANCEL_ENABLE, NULL);
    if (s != 0)
        handle_error_en(s, "apth_setcancelstate");

    /* sleep() is a cancelation point. */
    sleep(1000); /* Should get canceled while we sleep */

    /* Should never get here. */
    // printf("%s(): not canceled!\n", __func__);
    static char child_msg_3[] = "child: not canceled!\n";
    write(2, child_msg_3, sizeof(child_msg_3));
    return NULL;
}

/* ---- library configuration (optional) ---- */
/*
 * Override the number of worker threads.  Remove this block entirely to use
 * the library default (1 worker).  As apth_init_t gains more fields in
 * future releases, add extra assignments here — defaults for every other
 * field are already applied by apth_config_defaults() before these
 * statements run.
 */
APTH_CONFIG(cfg,
            cfg->workers = 1;)

/* ---- application entry point ---- */
/*
 * Everything between APTH_MAIN_BEGIN and APTH_MAIN_END is the body of a
 * static thread function; argc and argv are in scope just like a normal
 * main().  Variable declarations with commas (e.g. "int x, y;") work fine
 * because the body is not a macro argument.
 */
APTH_MAIN_BEGIN(argc, argv)
{
    apth_t thr;
    void *res;
    int s;

    /* Start a thread and then send it a cancelation request. */

    s = apth_create(&thr, NULL, &thread_func, NULL);
    if (s != 0)
        handle_error_en(s, "apth_create");

    sleep(2); /* Give thread a chance to get started */

    // printf("%s(): sending cancelation request\n", __func__);
    static char main_msg_1[] = "main: sending cancelation request\n";
    write(2, main_msg_1, sizeof(main_msg_1));

    s = apth_cancel(thr);
    if (s != 0)
        handle_error_en(s, "apth_cancel");

    /* Join with thread to see what its exit status was. */

    s = apth_join(thr, &res);
    if (s != 0)
        handle_error_en(s, "apth_join");

    static char main_msg_2[] = "main: thread was canceled\n";
    static char main_msg_3[] = "main: thread wasn't canceled (shouldn't happen!)\n";
    if (res == APTH_CANCELED)
        // printf("%s(): thread was canceled\n", __func__);
        write(2, main_msg_2, sizeof(main_msg_2));
    else
        // printf("%s(): thread wasn't canceled (shouldn't happen!)\n", __func__);
        write(2, main_msg_3, sizeof(main_msg_3));
}
APTH_MAIN_END
