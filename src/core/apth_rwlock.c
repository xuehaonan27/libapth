#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"
#include <malloc.h>

int apth_rwlock_init(apth_rwlock_t *rwlock, const void *attr)
{
    (void)attr;
    if (rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = (struct apth_rwlock_st *)malloc(sizeof(struct apth_rwlock_st));
    if (rw == NULL)
        return ENOMEM;

    int rc;
    rw->mtx = NULL;
    rw->rd_cv = NULL;
    rw->wr_cv = NULL;

    rc = apth_mutex_init(&rw->mtx, NULL);
    if (rc != 0)
    {
        free(rw);
        return rc;
    }

    rc = apth_cond_init(&rw->rd_cv, NULL);
    if (rc != 0)
    {
        apth_mutex_destroy(&rw->mtx);
        free(rw);
        return rc;
    }

    rc = apth_cond_init(&rw->wr_cv, NULL);
    if (rc != 0)
    {
        apth_cond_destroy(&rw->rd_cv);
        apth_mutex_destroy(&rw->mtx);
        free(rw);
        return rc;
    }

    rw->readers = 0;
    rw->writers = 0;
    rw->waiting_writers = 0;

    *rwlock = rw;
    return 0;
}

int apth_rwlock_destroy(apth_rwlock_t *rwlock)
{
    if (rwlock == NULL || *rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = *rwlock;

    if (rw->readers > 0 || rw->writers > 0 || rw->waiting_writers > 0)
        return EBUSY;

    apth_cond_destroy(&rw->wr_cv);
    apth_cond_destroy(&rw->rd_cv);
    apth_mutex_destroy(&rw->mtx);
    free(rw);
    *rwlock = NULL;
    return 0;
}

int apth_rwlock_rdlock(apth_rwlock_t *rwlock)
{
    if (rwlock == NULL || *rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = *rwlock;

    apth_mutex_lock(&rw->mtx);

    // Write-preferring: wait while there is an active or waiting writer
    while (rw->writers > 0 || rw->waiting_writers > 0)
        apth_cond_wait(&rw->rd_cv, &rw->mtx);

    rw->readers++;
    apth_mutex_unlock(&rw->mtx);

    return 0;
}

int apth_rwlock_timedrdlock(apth_rwlock_t *rwlock, const struct timespec *abstime)
{
    if (rwlock == NULL || *rwlock == NULL)
        return EINVAL;
    if (abstime == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = *rwlock;

    apth_mutex_lock(&rw->mtx);

    while (rw->writers > 0 || rw->waiting_writers > 0)
    {
        int rc = apth_cond_timedwait(&rw->rd_cv, &rw->mtx, abstime);
        if (rc == ETIMEDOUT)
        {
            apth_mutex_unlock(&rw->mtx);
            return ETIMEDOUT;
        }
    }

    rw->readers++;
    apth_mutex_unlock(&rw->mtx);

    return 0;
}

int apth_rwlock_tryrdlock(apth_rwlock_t *rwlock)
{
    if (rwlock == NULL || *rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = *rwlock;

    apth_mutex_lock(&rw->mtx);

    if (rw->writers > 0 || rw->waiting_writers > 0)
    {
        apth_mutex_unlock(&rw->mtx);
        return EBUSY;
    }

    rw->readers++;
    apth_mutex_unlock(&rw->mtx);

    return 0;
}

int apth_rwlock_wrlock(apth_rwlock_t *rwlock)
{
    if (rwlock == NULL || *rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = *rwlock;

    apth_mutex_lock(&rw->mtx);

    rw->waiting_writers++;
    while (rw->readers > 0 || rw->writers > 0)
        apth_cond_wait(&rw->wr_cv, &rw->mtx);
    rw->waiting_writers--;
    rw->writers = 1;

    apth_mutex_unlock(&rw->mtx);

    return 0;
}

int apth_rwlock_timedwrlock(apth_rwlock_t *rwlock, const struct timespec *abstime)
{
    if (rwlock == NULL || *rwlock == NULL)
        return EINVAL;
    if (abstime == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = *rwlock;

    apth_mutex_lock(&rw->mtx);

    rw->waiting_writers++;
    while (rw->readers > 0 || rw->writers > 0)
    {
        int rc = apth_cond_timedwait(&rw->wr_cv, &rw->mtx, abstime);
        if (rc == ETIMEDOUT)
        {
            rw->waiting_writers--;
            apth_mutex_unlock(&rw->mtx);
            return ETIMEDOUT;
        }
    }
    rw->waiting_writers--;
    rw->writers = 1;

    apth_mutex_unlock(&rw->mtx);

    return 0;
}

int apth_rwlock_trywrlock(apth_rwlock_t *rwlock)
{
    if (rwlock == NULL || *rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = *rwlock;

    apth_mutex_lock(&rw->mtx);

    if (rw->readers > 0 || rw->writers > 0)
    {
        apth_mutex_unlock(&rw->mtx);
        return EBUSY;
    }

    rw->writers = 1;
    apth_mutex_unlock(&rw->mtx);

    return 0;
}

int apth_rwlock_unlock(apth_rwlock_t *rwlock)
{
    if (rwlock == NULL || *rwlock == NULL)
        return EINVAL;

    struct apth_rwlock_st *rw = *rwlock;

    apth_mutex_lock(&rw->mtx);

    if (rw->writers > 0)
    {
        // Writer releasing
        rw->writers = 0;
        if (rw->waiting_writers > 0)
            apth_cond_signal(&rw->wr_cv);
        else
            apth_cond_broadcast(&rw->rd_cv);
    }
    else if (rw->readers > 0)
    {
        // Reader releasing
        rw->readers--;
        if (rw->readers == 0 && rw->waiting_writers > 0)
            apth_cond_signal(&rw->wr_cv);
    }

    apth_mutex_unlock(&rw->mtx);

    return 0;
}
