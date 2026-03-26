#ifndef __LIBAPTH_UTILS_APTH_SYSUTILS_H
#define __LIBAPTH_UTILS_APTH_SYSUTILS_H

#include "debug.h"
#include "apth_errno.h"

// Get number of online CPU cores
long cpu_cores(void);

// Get page size
long page_size(void);

#ifdef APTH_NUMA
// Get number of NUMA nodes on the system
int apth_numa_node_count(void);

// Get the NUMA node of a given CPU core
int apth_numa_node_of_cpu(int cpu);
#endif

#endif // __LIBAPTH_UTILS_APTH_SYSUTILS_H
