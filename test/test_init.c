// Test initializing the LIBAPTH package

#include "apth.h"

int main(void)
{
    apth_init_t initvals;
    apth_initvals_init(&initvals, 1);
    apth_init(&initvals);
    char a[] = "asdf\n";
    write(2, a, sizeof(a));
    apth_drop();
    return 0;
}
