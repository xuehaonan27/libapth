#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/apth_errno.h"
#include <malloc.h>

int apth_sem_init(apth_sem_t *sem, int pshared, unsigned int value)
{
    (void)pshared;
    if (sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = (struct apth_sem_st *)malloc(sizeof(struct apth_sem_st));
    if (s == NULL)
        return ENOMEM;

    int rc;
    s->mtx = NULL;
    s->cv = NULL;

    rc = apth_mutex_init(&s->mtx, NULL);
    if (rc != 0)
    {
        free(s);
        return rc;
    }

    rc = apth_cond_init(&s->cv, NULL);
    if (rc != 0)
    {
        apth_mutex_destroy(&s->mtx);
        free(s);
        return rc;
    }

    s->value = value;
    *sem = s;
    return 0;
}

int apth_sem_destroy(apth_sem_t *sem)
{
    if (sem == NULL || *sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = *sem;
    apth_cond_destroy(&s->cv);
    apth_mutex_destroy(&s->mtx);
    free(s);
    *sem = NULL;
    return 0;
}

int apth_sem_wait(apth_sem_t *sem)
{
    if (sem == NULL || *sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = *sem;

    apth_mutex_lock(&s->mtx);
    while (s->value == 0)
        apth_cond_wait(&s->cv, &s->mtx);
    s->value--;
    apth_mutex_unlock(&s->mtx);

    return 0;
}

int apth_sem_trywait(apth_sem_t *sem)
{
    if (sem == NULL || *sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = *sem;

    apth_mutex_lock(&s->mtx);
    if (s->value == 0)
    {
        apth_mutex_unlock(&s->mtx);
        return EAGAIN;
    }
    s->value--;
    apth_mutex_unlock(&s->mtx);

    return 0;
}

int apth_sem_post(apth_sem_t *sem)
{
    if (sem == NULL || *sem == NULL)
        return EINVAL;

    struct apth_sem_st *s = *sem;

    apth_mutex_lock(&s->mtx);
    s->value++;
    apth_cond_signal(&s->cv);
    apth_mutex_unlock(&s->mtx);

    return 0;
}

int apth_sem_getvalue(apth_sem_t *sem, int *sval)
{
    if (sem == NULL || *sem == NULL || sval == NULL)
        return EINVAL;

    struct apth_sem_st *s = *sem;

    apth_mutex_lock(&s->mtx);
    *sval = (int)s->value;
    apth_mutex_unlock(&s->mtx);

    return 0;
}
