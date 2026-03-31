#ifndef __LIBAPTH_UTILS_APTH_SYSUTILS_H
#define __LIBAPTH_UTILS_APTH_SYSUTILS_H

#include "debug.h"
#include "apth_errno.h"

// Get number of available CPU cores (respects cgroup cpuset)
long cpu_cores(void);

// Map worker_id (0-based) to actual CPU id from the allowed set.
// e.g., cgroup cpuset 40-59 → worker 0 maps to CPU 40.
int apth_worker_id_to_cpu(int worker_id);

// Get page size
long page_size(void);

#ifdef APTH_NUMA
// Get number of NUMA nodes on the system
int apth_numa_node_count(void);

// Get the NUMA node of a given CPU core
int apth_numa_node_of_cpu(int cpu);
#endif

#endif // __LIBAPTH_UTILS_APTH_SYSUTILS_H
