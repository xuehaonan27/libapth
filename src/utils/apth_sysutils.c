#include "apth_sysutils.h"
#include "apth_errno.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sched.h>
#include <sys/sysinfo.h>

// Cache for the allowed CPU set, populated once by cpu_cores().
static int _allowed_cpus[CPU_SETSIZE];
static int _allowed_cpu_count = 0;
static bool _allowed_cpus_initialized = false;

// Populate the allowed CPU cache from sched_getaffinity.
// This respects cgroup cpuset constraints.
static void _init_allowed_cpus(void)
{
    if (_allowed_cpus_initialized)
        return;

    cpu_set_t mask;
    CPU_ZERO(&mask);

    if (sched_getaffinity(0, sizeof(mask), &mask) != 0)
    {
        // Fallback: assume all CPUs 0..get_nprocs()-1
        int n = get_nprocs();
        for (int i = 0; i < n && i < CPU_SETSIZE; i++)
            _allowed_cpus[_allowed_cpu_count++] = i;
    }
    else
    {
        for (int i = 0; i < CPU_SETSIZE; i++)
        {
            if (CPU_ISSET(i, &mask))
                _allowed_cpus[_allowed_cpu_count++] = i;
        }
    }

    if (_allowed_cpu_count == 0)
        _allowed_cpus[_allowed_cpu_count++] = 0; // safety fallback

    _allowed_cpus_initialized = true;
}

long cpu_cores(void)
{
    _init_allowed_cpus();
    return _allowed_cpu_count;
}

// Map a worker_id (0-based) to the actual CPU id from the allowed set.
// e.g., if cgroup allows CPUs 40-59, worker 0 → CPU 40, worker 1 → CPU 41.
int apth_worker_id_to_cpu(int worker_id)
{
    _init_allowed_cpus();
    return _allowed_cpus[worker_id % _allowed_cpu_count];
}

long page_size(void)
{
    long ps;
    apth_shield
    {
        ps = sysconf(_SC_PAGE_SIZE);
    }
    assert_msg(ps != -1, "Fail to get page size");
    return ps;
}

#ifdef APTH_NUMA

/* Detect NUMA topology from /sys/devices/system/node/.
 * This avoids a libnuma dependency. */

int apth_numa_node_count(void)
{
    /* Count directories matching /sys/devices/system/node/node[0-9]+ */
    int count = 0;
    char path[128];
    for (int i = 0; i < 256; i++)
    {
        snprintf(path, sizeof(path), "/sys/devices/system/node/node%d", i);
        if (access(path, F_OK) == 0)
            count++;
        else
            break; /* NUMA nodes are numbered contiguously starting at 0 */
    }
    return count > 0 ? count : 1; /* At least 1 node (UMA fallback) */
}

int apth_numa_node_of_cpu(int cpu)
{
    /* Read /sys/devices/system/cpu/cpu<N>/topology/physical_package_id
     * or iterate /sys/devices/system/node/node<M>/cpulist */
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);

    FILE *f = fopen(path, "r");
    if (f == NULL)
    {
        /* Fallback: scan node cpulists */
        int num_nodes = apth_numa_node_count();
        for (int node = 0; node < num_nodes; node++)
        {
            snprintf(path, sizeof(path),
                     "/sys/devices/system/node/node%d/cpulist", node);
            f = fopen(path, "r");
            if (f == NULL)
                continue;

            /* Parse cpulist format: "0-3,8-11" */
            char buf[256];
            if (fgets(buf, sizeof(buf), f) != NULL)
            {
                /* Simple parser: handle ranges "A-B" and singles "A" */
                char *p = buf;
                while (*p)
                {
                    int lo = (int)strtol(p, &p, 10);
                    int hi = lo;
                    if (*p == '-')
                    {
                        p++;
                        hi = (int)strtol(p, &p, 10);
                    }
                    if (cpu >= lo && cpu <= hi)
                    {
                        fclose(f);
                        return node;
                    }
                    if (*p == ',')
                        p++;
                }
            }
            fclose(f);
        }
        return 0; /* Default to node 0 */
    }

    int node = 0;
    if (fscanf(f, "%d", &node) != 1)
        node = 0;
    fclose(f);
    return node;
}

#endif /* APTH_NUMA */