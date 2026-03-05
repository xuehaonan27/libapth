#include "apth.h"
#include <stdio.h>
#include <assert.h>

// Test that the new opaque union types can be stack-allocated and initialized

APTH_CONFIG(cfg, cfg->workers = 1;)

void *test_thread(void *arg)
{
    (void)arg;
    printf("Test thread running\n");
    return NULL;
}

APTH_MAIN_BEGIN(argc, argv)
    (void)argc;
    (void)argv;

    printf("Testing opaque union types...\n");

    // Test mutex
    apth_mutex_t mutex = APTH_MUTEX_INITIALIZER;
    assert(apth_mutex_init(&mutex, NULL) == 0);
    assert(apth_mutex_lock(&mutex) == 0);
    assert(apth_mutex_unlock(&mutex) == 0);
    assert(apth_mutex_destroy(&mutex) == 0);
    printf("✓ Mutex test passed\n");

    // Test condition variable
    apth_cond_t cond = APTH_COND_INITIALIZER;
    assert(apth_cond_init(&cond, NULL) == 0);
    assert(apth_cond_destroy(&cond) == 0);
    printf("✓ Condition variable test passed\n");

    // Test semaphore
    apth_sem_t sem = APTH_SEM_INITIALIZER;
    assert(apth_sem_init(&sem, 0, 1) == 0);
    assert(apth_sem_wait(&sem) == 0);
    assert(apth_sem_post(&sem) == 0);
    assert(apth_sem_destroy(&sem) == 0);
    printf("✓ Semaphore test passed\n");

    // Test barrier
    apth_barrier_t barrier = APTH_BARRIER_INITIALIZER;
    assert(apth_barrier_init(&barrier, NULL, 2) == 0);
    assert(apth_barrier_destroy(&barrier) == 0);
    printf("✓ Barrier test passed\n");

    // Test rwlock
    apth_rwlock_t rwlock = APTH_RWLOCK_INITIALIZER;
    assert(apth_rwlock_init(&rwlock, NULL) == 0);
    assert(apth_rwlock_rdlock(&rwlock) == 0);
    assert(apth_rwlock_unlock(&rwlock) == 0);
    assert(apth_rwlock_destroy(&rwlock) == 0);
    printf("✓ Read-write lock test passed\n");

    printf("\nAll tests passed! Opaque union types work correctly.\n");
    printf("Types can now be stack-allocated like pthread types.\n");

    exit(0);
APTH_MAIN_END
