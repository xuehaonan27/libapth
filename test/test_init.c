// Test initializing the LIBAPTH package

#include "apth.h"

int main(void)
{
    apth_init();
    char a[] = "asdf";
    write(2, a, sizeof(a));
    apth_drop();
    return 0;
}
