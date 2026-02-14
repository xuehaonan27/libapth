#ifndef __LIBAPTH_UTILS_APTH_SYSUTILS_H
#define __LIBAPTH_UTILS_APTH_SYSUTILS_H

#include "debug.h"
#include "apth_errno.h"
#include <unistd.h>

// Get number of online CPU cores
long cpu_cores(void);

// Get page size
long page_size(void);

#endif // __LIBAPTH_UTILS_APTH_SYSUTILS_H
