#include "apth.h"
#include <stdio.h>

#define NULL ((void *)0)

static char main_hi[] = "Hi from main apth\n";
static char child_hi[] = "Hi from child apth\n";
static char child_result[] = "Result from child apth\n";

void *child_apth(void *arg)
{
    char *s = (char *)arg;
    write(2, s, sizeof(child_hi));
    return (void *)child_result;
}

void *apth_main(void *arg)
{
    char *s = (char *)arg;
    write(2, s, sizeof(main_hi));

    apth_t child_th;
    apth_attr_t child_attr;
    apth_attr_init(&child_attr);
    apth_attr_setname_np(&child_attr, "child apth");
    apth_create(&child_th, &child_attr, child_apth, (void *)child_hi);

    void *cdata;
    apth_join(child_th, &cdata);
    write(2, cdata, sizeof(child_result));

    return NULL;
}

int main(void)
{
    apth_init_t initvals;
    apth_initvals_init(&initvals, 1, apth_main, (void *)main_hi);
    apth_init(&initvals);

    // `apth_init` should have called `pthread_exit` and end itself
    perror("Should not reach here");
    return 0;
}