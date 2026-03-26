#include "hook_time.h"
#include "internal/apth_event.h"
#include "internal/apth_time.h"
#include "internal/types.h"

// APTH variant of nanosleep(2)
APTH_DEFINE_HOOK(
    int, nanosleep,
    (const struct timespec *rqtp, struct timespec *rmtp),
    (rqtp, rmtp))
{
    {
        apth_t __ded_cur = CUR_APTH;
        if (__ded_cur != NULL && __ded_cur->is_dedicated)
            return apth_func_raw(nanosleep)(rqtp, rmtp);
    }
    apth_time_t until;
    apth_time_t offset;
    apth_time_t now;

    // Consistency checks for POSIX conformance
    if (rqtp == NULL)
        return apth_error(-1, EFAULT);
    if (rqtp->tv_nsec < 0 || rqtp->tv_nsec > (1000 * 1000000))
        return apth_error(-1, EINVAL);

    // Short-circuit
    if (rqtp->tv_sec == 0 && rqtp->tv_nsec == 0)
        return 0;

    // Calculate asleep time
    offset = apth_time((long)(rqtp->tv_sec), (long)(rqtp->tv_nsec) / 1000);
    apth_time_set(&until, APTH_TIME_NOW);
    apth_time_add(&until, &offset);

    // And let apth sleeps until this time is elapsed
    struct apth_event_st ev = EVENT_TIME(until);
    apth_wait_event(&ev);

    // Optionally provide amount of slept time
    if (rmtp != NULL)
    {
        apth_time_set(&now, APTH_TIME_NOW);
        apth_time_sub(&until, &now);
        rmtp->tv_sec = until.tv_sec;
        rmtp->tv_nsec = until.tv_usec * 1000;
    }

    return 0;
}

// APTH variant of usleep(3)
APTH_DEFINE_HOOK(int, usleep, (unsigned int usec), (usec))
{
    {
        apth_t __ded_cur = CUR_APTH;
        if (__ded_cur != NULL && __ded_cur->is_dedicated)
            return apth_func_raw(usleep)(usec);
    }
    apth_time_t until;
    apth_time_t offset;

    // Short-circuit
    if (usec == 0)
        return 0;

    // Calculate asleep time
    offset = apth_time((long)(usec / 1000000), (long)(usec % 1000000));
    apth_time_set(&until, APTH_TIME_NOW);
    apth_time_add(&until, &offset);

    // and let thread sleep until this time is elapsed
    struct apth_event_st ev = EVENT_TIME(until);
    apth_wait_event(&ev);

    return 0;
}

APTH_DEFINE_HOOK(unsigned int, sleep, (unsigned int sec), (sec))
{
    {
        apth_t __ded_cur = CUR_APTH;
        if (__ded_cur != NULL && __ded_cur->is_dedicated)
            return apth_func_raw(sleep)(sec);
    }
    apth_time_t until;
    apth_time_t offset;

    // Consistency check
    if (sec == 0)
        return 0;

    // Calculate asleep time
    offset = apth_time(sec, 0);
    apth_time_set(&until, APTH_TIME_NOW);
    apth_time_add(&until, &offset);

    // And let thread sleep until this time is elapsed
    struct apth_event_st ev = EVENT_TIME(until);
    apth_wait_event(&ev);

    return 0;
}
