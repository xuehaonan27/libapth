#include "internal_types.h"
#include "internal_funcs.h"

apth_time_t apth_time_zero = {0L, 0L};

APTH_INTERNAL uint64_t cpu_tick()
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi));
    return (((uint64_t)lo) | (((uint64_t)hi) << 32));
}

APTH_INTERNAL apth_time_t apth_time(long sec, long usec)
{
    apth_time_t tv;
    tv.tv_sec = sec;
    tv.tv_usec = usec;
    return tv;
}

APTH_INTERNAL apth_time_t apth_timeout(long sec, long usec)
{
    apth_time_t tv;
    apth_time_t tvd;

    apth_time_set(&tv, APTH_TIME_NOW);
    tvd.tv_sec = sec;
    tvd.tv_usec = usec;
    apth_time_add(&tv, &tvd);
    return tv;
}

APTH_INTERNAL void apth_time_set(apth_time_t *t1, apth_time_t *t2)
{
    if (t2 == APTH_TIME_NOW)
        gettimeofday(t1, NULL);
    else
    {
        t1->tv_sec = t2->tv_sec;
        t1->tv_usec = t2->tv_usec;
    }
}

APTH_INTERNAL void apth_time_add(apth_time_t *t1, apth_time_t *t2)
{
    t1->tv_sec += t2->tv_sec;
    t1->tv_usec += t2->tv_usec;
    if (t1->tv_usec > 1000000)
    {
        t1->tv_sec += 1;
        t1->tv_usec -= 1000000;
    }
}

APTH_INTERNAL void apth_time_sub(apth_time_t *t1, apth_time_t *t2)
{
    t1->tv_sec -= t2->tv_sec;
    t1->tv_usec -= t2->tv_usec;
    if (t1->tv_usec < 0)
    {
        t1->tv_sec -= 1;
        t1->tv_usec += 1000000;
    }
}

APTH_INTERNAL int apth_time_cmp(apth_time_t *t1, apth_time_t *t2)
{
    int rc;

    rc = t1->tv_sec - t2->tv_sec;
    if (rc == 0)
        rc = t1->tv_usec - t2->tv_usec;
    return rc;
}

APTH_INTERNAL double apth_time_t2d(apth_time_t *t)
{
    double d;

    d = ((double)t->tv_sec * 1000000 + (double)t->tv_usec) / 1000000;
    return d;
}
