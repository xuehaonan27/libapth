#include "apth.h"
#include <stdio.h>
#include <assert.h>

// Test that all opaque union types work correctly

APTH_CONFIG(cfg, cfg->workers = 1;)

void *test_thread(void *arg)
{
    (void)arg;
    printf("Test thread running\n");
    return NULL;
}

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    printf("Testing all opaque union types...\n\n");

    // Test thread attributes
    printf("Testing apth_attr_t...\n");
    apth_attr_t attr;
    assert(apth_attr_init(&attr) == 0);
    assert(apth_attr_setdetachstate(&attr, APTH_CREATE_JOINABLE) == 0);
    int detachstate;
    assert(apth_attr_getdetachstate(&attr, &detachstate) == 0);
    assert(detachstate == APTH_CREATE_JOINABLE);
    assert(apth_attr_destroy(&attr) == 0);
    printf("✓ Thread attributes test passed\n");

    // Test creating a thread with attributes
    printf("Testing thread creation with attributes...\n");
    apth_attr_t thread_attr;
    assert(apth_attr_init(&thread_attr) == 0);
    assert(apth_attr_setname_np(&thread_attr, "test_thread") == 0);

    apth_t thread;
    assert(apth_create(&thread, &thread_attr, test_thread, NULL) == 0);
    assert(apth_join(thread, NULL) == 0);
    assert(apth_attr_destroy(&thread_attr) == 0);
    printf("✓ Thread creation with attributes test passed\n");

    // Test mutex
    printf("Testing apth_mutex_t...\n");
    apth_mutex_t mutex = APTH_MUTEX_INITIALIZER;
    assert(apth_mutex_init(&mutex, NULL) == 0);
    assert(apth_mutex_lock(&mutex) == 0);
    assert(apth_mutex_unlock(&mutex) == 0);
    assert(apth_mutex_destroy(&mutex) == 0);
    printf("✓ Mutex test passed\n");

    // Test condition variable
    printf("Testing apth_cond_t...\n");
    apth_cond_t cond = APTH_COND_INITIALIZER;
    assert(apth_cond_init(&cond, NULL) == 0);
    assert(apth_cond_destroy(&cond) == 0);
    printf("✓ Condition variable test passed\n");

    // Test semaphore
    printf("Testing apth_sem_t...\n");
    apth_sem_t sem = APTH_SEM_INITIALIZER;
    assert(apth_sem_init(&sem, 0, 1) == 0);
    assert(apth_sem_wait(&sem) == 0);
    assert(apth_sem_post(&sem) == 0);
    assert(apth_sem_destroy(&sem) == 0);
    printf("✓ Semaphore test passed\n");

    // Test barrier
    printf("Testing apth_barrier_t...\n");
    apth_barrier_t barrier = APTH_BARRIER_INITIALIZER;
    assert(apth_barrier_init(&barrier, NULL, 2) == 0);
    assert(apth_barrier_destroy(&barrier) == 0);
    printf("✓ Barrier test passed\n");

    // Test rwlock
    printf("Testing apth_rwlock_t...\n");
    apth_rwlock_t rwlock = APTH_RWLOCK_INITIALIZER;
    assert(apth_rwlock_init(&rwlock, NULL) == 0);
    assert(apth_rwlock_rdlock(&rwlock) == 0);
    assert(apth_rwlock_unlock(&rwlock) == 0);
    assert(apth_rwlock_destroy(&rwlock) == 0);
    printf("✓ Read-write lock test passed\n");

    printf("\n===========================================\n");
    printf("All tests passed! All opaque union types work correctly.\n");
    printf("All types can now be stack-allocated like pthread types.\n");
    printf("===========================================\n");

    exit(0);
}
APTH_MAIN_END
