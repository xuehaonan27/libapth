#include <stdio.h>
#include "apth.h"
#include "core/apth_barrier.h"
#include "core/apth_cond.h"
#include "core/apth_mutex.h"
#include "core/apth_rwlock.h"
#include "core/apth_sem.h"

int main()
{
    printf("Opaque type sizes (from apth.h):\n");
    printf("  apth_sem_t:     %zu bytes (allocated: %d)\n", sizeof(apth_sem_t), __SIZEOF_APTH_SEM_T);
    printf("  apth_barrier_t: %zu bytes (allocated: %d)\n", sizeof(apth_barrier_t), __SIZEOF_APTH_BARRIER_T);
    printf("  apth_rwlock_t:  %zu bytes (allocated: %d)\n", sizeof(apth_rwlock_t), __SIZEOF_APTH_RWLOCK_T);
    printf("  apth_mutex_t:   %zu bytes (allocated: %d)\n", sizeof(apth_mutex_t), __SIZEOF_APTH_MUTEX_T);
    printf("  apth_cond_t:    %zu bytes (allocated: %d)\n", sizeof(apth_cond_t), __SIZEOF_APTH_COND_T);

    printf("\nInternal structure sizes:\n");
    printf("  struct apth_sem_st:     %zu bytes\n", sizeof(struct apth_sem_st));
    printf("  struct apth_barrier_st: %zu bytes\n", sizeof(struct apth_barrier_st));
    printf("  struct apth_rwlock_st:  %zu bytes\n", sizeof(struct apth_rwlock_st));
    printf("  struct apth_mutex_st:   %zu bytes\n", sizeof(struct apth_mutex_st));
    printf("  struct apth_cond_st:    %zu bytes\n", sizeof(struct apth_cond_st));

    printf("\nComponent sizes:\n");
    printf("  lll_apth_t:     %zu bytes\n", sizeof(lll_apth_t));
    printf("  lll_internal_t: %zu bytes\n", sizeof(lll_internal_t));
    printf("  struct list:    %zu bytes\n", sizeof(struct list));

    printf("\nSize check results:\n");
    int all_ok = 1;

    if (sizeof(struct apth_sem_st) <= sizeof(apth_sem_t))
    {
        printf("  ✓ Semaphore: OK (%zu <= %zu)\n", sizeof(struct apth_sem_st), sizeof(apth_sem_t));
    }
    else
    {
        printf("  ✗ Semaphore: FAIL (%zu > %zu)\n", sizeof(struct apth_sem_st), sizeof(apth_sem_t));
        all_ok = 0;
    }

    if (sizeof(struct apth_barrier_st) <= sizeof(apth_barrier_t))
    {
        printf("  ✓ Barrier: OK (%zu <= %zu)\n", sizeof(struct apth_barrier_st), sizeof(apth_barrier_t));
    }
    else
    {
        printf("  ✗ Barrier: FAIL (%zu > %zu)\n", sizeof(struct apth_barrier_st), sizeof(apth_barrier_t));
        all_ok = 0;
    }

    if (sizeof(struct apth_rwlock_st) <= sizeof(apth_rwlock_t))
    {
        printf("  ✓ RWLock: OK (%zu <= %zu)\n", sizeof(struct apth_rwlock_st), sizeof(apth_rwlock_t));
    }
    else
    {
        printf("  ✗ RWLock: FAIL (%zu > %zu)\n", sizeof(struct apth_rwlock_st), sizeof(apth_rwlock_t));
        all_ok = 0;
    }

    if (sizeof(struct apth_mutex_st) <= sizeof(apth_mutex_t))
    {
        printf("  ✓ Mutex: OK (%zu <= %zu)\n", sizeof(struct apth_mutex_st), sizeof(apth_mutex_t));
    }
    else
    {
        printf("  ✗ Mutex: FAIL (%zu > %zu)\n", sizeof(struct apth_mutex_st), sizeof(apth_mutex_t));
        all_ok = 0;
    }

    if (sizeof(struct apth_cond_st) <= sizeof(apth_cond_t))
    {
        printf("  ✓ Condvar: OK (%zu <= %zu)\n", sizeof(struct apth_cond_st), sizeof(apth_cond_t));
    }
    else
    {
        printf("  ✗ Condvar: FAIL (%zu > %zu)\n", sizeof(struct apth_cond_st), sizeof(apth_cond_t));
        all_ok = 0;
    }

    return all_ok ? 0 : 1;
}
