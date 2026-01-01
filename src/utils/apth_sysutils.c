#include "apth_sysutils.h"
#include "apth_errno.h"

long cpu_cores(void)
{
    // TODO: using APTH wrapped syscall
    long cpu_cores;
    apth_shield
    {
        cpu_cores = sysconf(_SC_NPROCESSORS_ONLN);
        assert_msg(cpu_cores != -1, "Fail to get CPU cores, errno = %d", errno);
    }
    return cpu_cores;
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