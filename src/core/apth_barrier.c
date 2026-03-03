#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"
#include <malloc.h>

int apth_barrier_init(apth_barrier_t *barrier, const void *attr, unsigned int count)
{
    (void)attr;
    if (barrier == NULL || count == 0)
        return EINVAL;

    struct apth_barrier_st *b = (struct apth_barrier_st *)malloc(sizeof(struct apth_barrier_st));
    if (b == NULL)
        return ENOMEM;

    int rc;
    b->mtx = NULL;
    b->cv = NULL;

    rc = apth_mutex_init(&b->mtx, NULL);
    if (rc != 0)
    {
        free(b);
        return rc;
    }

    rc = apth_cond_init(&b->cv, NULL);
    if (rc != 0)
    {
        apth_mutex_destroy(&b->mtx);
        free(b);
        return rc;
    }

    b->threshold = count;
    b->count = 0;
    b->generation = 0;

    *barrier = b;
    return 0;
}

int apth_barrier_destroy(apth_barrier_t *barrier)
{
    if (barrier == NULL || *barrier == NULL)
        return EINVAL;

    struct apth_barrier_st *b = *barrier;

    // If threads are still waiting, cannot destroy
    if (b->count > 0)
        return EBUSY;

    apth_cond_destroy(&b->cv);
    apth_mutex_destroy(&b->mtx);
    free(b);
    *barrier = NULL;
    return 0;
}

int apth_barrier_wait(apth_barrier_t *barrier)
{
    if (barrier == NULL || *barrier == NULL)
        return EINVAL;

    struct apth_barrier_st *b = *barrier;

    apth_mutex_lock(&b->mtx);

    unsigned int gen = b->generation;
    b->count++;

    if (b->count == b->threshold)
    {
        // Last thread to arrive: reset and wake all
        b->count = 0;
        b->generation++;
        apth_cond_broadcast(&b->cv);
        apth_mutex_unlock(&b->mtx);
        return APTH_BARRIER_SERIAL_THREAD;
    }

    // Wait until generation changes
    while (b->generation == gen)
        apth_cond_wait(&b->cv, &b->mtx);

    apth_mutex_unlock(&b->mtx);
    return 0;
}
