// #include <errno.h>
// #include <pthread.h>
// #include <stdio.h>
// #include <stdlib.h>
// #include <unistd.h>

#include "apth.h"
#include <errno.h>

#define NULL ((void *)0)
#define EXIT_SUCCESS (0)
#define EXIT_FAILURE (1)

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
    int s;

    /* Disable cancelation for a while, so that we don't
       immediately react to a cancelation request. */

    s = apth_setcancelstate(APTH_CANCEL_DISABLE, NULL);
    if (s != 0)
        handle_error_en(s, "apth_setcancelstate");

    printf("%s(): started; cancelation disabled\n", __func__);
    sleep(5);
    printf("%s(): about to enable cancelation\n", __func__);

    s = apth_setcancelstate(APTH_CANCEL_ENABLE, NULL);
    if (s != 0)
        handle_error_en(s, "apth_setcancelstate");

    /* sleep() is a cancelation point. */

    sleep(1000); /* Should get canceled while we sleep */

    /* Should never get here. */

    printf("%s(): not canceled!\n", __func__);
    return NULL;
}

void *apth_main(void *)
{
    apth_t thr;
    void *res;
    int s;

    /* Start a thread and then send it a cancelation request. */

    s = apth_create(&thr, NULL, &thread_func, NULL);
    if (s != 0)
        handle_error_en(s, "apth_create");

    sleep(2); /* Give thread a chance to get started */

    printf("%s(): sending cancelation request\n", __func__);
    s = apth_cancel(thr);
    if (s != 0)
        handle_error_en(s, "apth_cancel");

    /* Join with thread to see what its exit status was. */

    s = apth_join(thr, &res);
    if (s != 0)
        handle_error_en(s, "apth_join");

    if (res == APTH_CANCELED)
        printf("%s(): thread was canceled\n", __func__);
    else
        printf("%s(): thread wasn't canceled (shouldn't happen!)\n",
               __func__);
    exit(EXIT_SUCCESS);
}

int main(void)
{
    apth_init_t initvals;
    apth_initvals_init(&initvals, 1, apth_main, NULL);
    apth_init(&initvals);

    // `apth_init` should have called `pthread_exit` and end itself
    perror("Should not reach here");
    return 0;
}