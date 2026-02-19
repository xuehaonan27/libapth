#include "apth.h"
#include <stdio.h>

#define NULL ((void *)0)

static const char msg[] = "Hello from child\n";

void *apth_main(void *arg) {
    char *s = (char *)arg;
    write(2, s, sizeof(msg));
    return NULL;
}

int main(void) {
    apth_init_t initvals;
    apth_initvals_init(&initvals, 2, apth_main, (void *)msg);
    apth_init(&initvals);

    // `apth_init` should have called `pthread_exit` and end itself
    perror("Should not reach here");
    return 0;
}
