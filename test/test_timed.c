#include "apth.h"
#include "utils/apth_string.h"
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>

#define N_WORKERS 2

static apth_mutex_t mtx;
static apth_cond_t cv;
static apth_rwlock_t rwl;
static apth_sem_t sem;

// Helper: get an absolute deadline `ms` milliseconds from now.
static struct timespec deadline_ms(int ms)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    struct timespec ts;
    ts.tv_sec = now.tv_sec + ms / 1000;
    ts.tv_nsec = now.tv_usec * 1000 + (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

// Helper: get an absolute deadline that is already in the past.
static struct timespec deadline_past(void)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    return ts;
}

static volatile int cond_flag = 0;

static void *
cond_signaler(void *arg)
{
    (void)arg;
    // Small delay then signal
    for (volatile int i = 0; i < 50000; i++)
        ;
    apth_mutex_lock(&mtx);
    cond_flag = 1;
    apth_cond_signal(&cv);
    apth_mutex_unlock(&mtx);
    return NULL;
}

static void *
sem_poster(void *arg)
{
    (void)arg;
    for (volatile int i = 0; i < 50000; i++)
        ;
    apth_sem_post(&sem);
    return NULL;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char msg[] = "test_timed: starting\n";
    write(2, msg, sizeof(msg) - 1);

    int failures = 0;
    int rc;
    struct timespec ts;

    // ==================== Mutex timedlock ====================

    apth_mutex_init(&mtx, NULL);
    apth_cond_init(&cv, NULL);

    // 1. timedlock on free mutex should succeed
    ts = deadline_ms(100);
    rc = apth_mutex_timedlock(&mtx, &ts);
    if (rc != 0)
    {
        static char err[] = "test_timed: mutex timedlock on free mutex failed\n";
        write(2, err, sizeof(err) - 1);
        failures++;
    }

    // 2. timedlock on locked mutex with past deadline should return ETIMEDOUT
    ts = deadline_past();
    rc = apth_mutex_timedlock(&mtx, &ts);
    if (rc != ETIMEDOUT)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_timed: mutex timedlock expected ETIMEDOUT got %d\n", rc);
        write(2, buf, len);
        failures++;
    }

    // 3. timedlock on locked mutex with short timeout should return ETIMEDOUT
    ts = deadline_ms(50);
    rc = apth_mutex_timedlock(&mtx, &ts);
    if (rc != ETIMEDOUT)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_timed: mutex timedlock short timeout expected ETIMEDOUT got %d\n", rc);
        write(2, buf, len);
        failures++;
    }

    apth_mutex_unlock(&mtx);

    // ==================== Cond timedwait ====================

    // 4. timedwait with past deadline should return ETIMEDOUT
    apth_mutex_lock(&mtx);
    ts = deadline_past();
    rc = apth_cond_timedwait(&cv, &mtx, &ts);
    if (rc != ETIMEDOUT)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_timed: cond timedwait expected ETIMEDOUT got %d\n", rc);
        write(2, buf, len);
        failures++;
    }
    apth_mutex_unlock(&mtx);

    // 5. timedwait with signal before deadline should return 0
    cond_flag = 0;
    apth_t signaler;
    apth_attr_t attr;
    apth_attr_init(&attr);
    apth_attr_setstacksize(&attr, 4096);

    apth_create(&signaler, &attr, cond_signaler, NULL);

    apth_mutex_lock(&mtx);
    ts = deadline_ms(5000); // generous deadline
    while (cond_flag == 0)
    {
        rc = apth_cond_timedwait(&cv, &mtx, &ts);
        if (rc == ETIMEDOUT)
            break;
    }
    apth_mutex_unlock(&mtx);

    if (cond_flag != 1)
    {
        static char err[] = "test_timed: cond timedwait signal not received\n";
        write(2, err, sizeof(err) - 1);
        failures++;
    }

    apth_join(signaler, NULL);

    // ==================== Semaphore timedwait ====================

    apth_sem_init(&sem, 0, 0);

    // 6. timedwait on zero sem with past deadline should return ETIMEDOUT
    ts = deadline_past();
    rc = apth_sem_timedwait(&sem, &ts);
    if (rc != ETIMEDOUT)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_timed: sem timedwait expected ETIMEDOUT got %d\n", rc);
        write(2, buf, len);
        failures++;
    }

    // 7. timedwait with post before deadline should succeed
    apth_t poster;
    apth_create(&poster, &attr, sem_poster, NULL);

    ts = deadline_ms(5000);
    rc = apth_sem_timedwait(&sem, &ts);
    if (rc != 0)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_timed: sem timedwait expected 0 got %d\n", rc);
        write(2, buf, len);
        failures++;
    }

    apth_join(poster, NULL);
    apth_sem_destroy(&sem);

    // ==================== RWLock timedrdlock ====================

    apth_rwlock_init(&rwl, NULL);

    // 8. timedrdlock on free lock should succeed
    ts = deadline_ms(100);
    rc = apth_rwlock_timedrdlock(&rwl, &ts);
    if (rc != 0)
    {
        static char err[] = "test_timed: rwlock timedrdlock on free lock failed\n";
        write(2, err, sizeof(err) - 1);
        failures++;
    }
    apth_rwlock_unlock(&rwl);

    // 9. timedwrlock on free lock should succeed
    ts = deadline_ms(100);
    rc = apth_rwlock_timedwrlock(&rwl, &ts);
    if (rc != 0)
    {
        static char err[] = "test_timed: rwlock timedwrlock on free lock failed\n";
        write(2, err, sizeof(err) - 1);
        failures++;
    }

    // 10. timedrdlock while write-locked with past deadline should ETIMEDOUT
    ts = deadline_past();
    rc = apth_rwlock_timedrdlock(&rwl, &ts);
    if (rc != ETIMEDOUT)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_timed: rwlock timedrdlock expected ETIMEDOUT got %d\n", rc);
        write(2, buf, len);
        failures++;
    }

    // 11. timedwrlock while write-locked with past deadline should ETIMEDOUT
    ts = deadline_past();
    rc = apth_rwlock_timedwrlock(&rwl, &ts);
    if (rc != ETIMEDOUT)
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_timed: rwlock timedwrlock expected ETIMEDOUT got %d\n", rc);
        write(2, buf, len);
        failures++;
    }

    apth_rwlock_unlock(&rwl);
    apth_rwlock_destroy(&rwl);

    // ==================== Cleanup ====================

    apth_attr_destroy(&attr);
    apth_cond_destroy(&cv);
    apth_mutex_destroy(&mtx);

    if (failures == 0)
    {
        static char ok[] = "test_timed: PASS\n";
        write(2, ok, sizeof(ok) - 1);
    }
    else
    {
        char buf[128];
        int len = apth_snprintf(buf, sizeof(buf),
                                "test_timed: FAIL (%d failures)\n", failures);
        write(2, buf, len);
        exit(EXIT_FAILURE);
    }
}
APTH_MAIN_END
