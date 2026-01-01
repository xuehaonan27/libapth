#include "internal_types.h"

inline uint64_t cpu_tick()
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi));
    return (((uint64_t)lo) | (((uint64_t)hi) << 32));
}

apth_time_t apth_time(long sec, long usec)
{
    apth_time_t tv;
    tv.tv_sec = sec;
    tv.tv_usec = usec;
    return tv;
}
