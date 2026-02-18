// Test initializing the LIBAPTH package

#include "apth.h"
#include <stdio.h>

#define NULL ((void *)0)

static char a[] = "asdf\n";

void *apth_main(void *arg)
{
    char *s = (char *)arg;
    write(2, s, sizeof(a));
    return NULL;
}

int main(void)
{
    apth_init_t initvals;
    apth_initvals_init(&initvals, 1, apth_main, (void *)a);
    apth_init(&initvals);

    // `apth_init` should have called `pthread_exit` and end itself
    perror("Should not reach here");
    return 0;
}
