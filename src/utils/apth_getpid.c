#include "apth_getpid.h"
#include <unistd.h>

APTH_INTERNAL pid_t __apth_getpid(void) {
    return getpid();
}
