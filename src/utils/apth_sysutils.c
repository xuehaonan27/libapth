#include "apth_sysutils.h"
#include "apth_errno.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysinfo.h>

long cpu_cores(void)
{
    // TODO: using APTH wrapped syscall
    /* long cpu_cores;
    apth_shield
    {
        cpu_cores = sysconf(_SC_NPROCESSORS_ONLN);
        assert_msg(cpu_cores != -1, "Fail to get CPU cores, errno = %d", errno);
    }
    return cpu_cores;*/
    return get_nprocs();
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