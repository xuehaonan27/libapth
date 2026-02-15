// Test initializing the LIBAPTH package

#include "apth.h"

#define NULL ((void *)0)

static char a[] = "asdf\n";

void *child_thread(void *arg)
{
    write(2, a, sizeof(a));
}

int main(void)
{
    apth_init_t initvals;
    apth_initvals_init(&initvals, 1, child_thread, NULL);
    apth_init(&initvals);

    // apth_t th1;
    // apth_create(&th1, NULL, child_thread, NULL);

    apth_drop();
    return 0;
}
