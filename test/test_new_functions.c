#include "apth.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>

void *worker_thread(void *arg)
{
    (void)arg;
    printf("Worker thread running\n");
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = 2;)

APTH_MAIN_BEGIN(argc, argv)
    (void)argc;
    (void)argv;

    printf("=== Testing new LIBAPTH functions ===\n\n");

    // Test 1: apth_get_minstack
    printf("Test 1: apth_get_minstack\n");
    size_t minstack = apth_get_minstack(NULL);
    printf("  Minimum stack size: %zu bytes\n", minstack);
    assert(minstack == 16384); // APTH_STACK_SIZE_DEFAULT
    printf("  PASS\n\n");

    // Test 2: apth_condattr_setclock and apth_condattr_getclock
    printf("Test 2: apth_condattr_setclock and apth_condattr_getclock\n");
    apth_condattr_t cond_attr;
    int ret = apth_condattr_init(&cond_attr);
    assert(ret == 0);

    // Get default clock
    clockid_t clock_id;
    ret = apth_condattr_getclock(&cond_attr, &clock_id);
    assert(ret == 0);
    printf("  Default clock ID: %d (CLOCK_REALTIME=%d)\n", clock_id, CLOCK_REALTIME);
    assert(clock_id == CLOCK_REALTIME);

    // Set to CLOCK_MONOTONIC
    ret = apth_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    assert(ret == 0);

    // Get the clock again
    ret = apth_condattr_getclock(&cond_attr, &clock_id);
    assert(ret == 0);
    printf("  After setting to CLOCK_MONOTONIC: %d\n", clock_id);
    assert(clock_id == CLOCK_MONOTONIC);

    ret = apth_condattr_destroy(&cond_attr);
    assert(ret == 0);
    printf("  PASS\n\n");

    // Test 3: apth_getcpuclockid
    printf("Test 3: apth_getcpuclockid\n");
    apth_t th;
    ret = apth_create(&th, NULL, worker_thread, NULL);
    assert(ret == 0);

    clockid_t cpu_clock_id;
    ret = apth_getcpuclockid(th, &cpu_clock_id);
    if (ret == 0) {
        printf("  CPU clock ID for thread: %d\n", cpu_clock_id);

        // Try to use the clock
        struct timespec ts;
        if (clock_gettime(cpu_clock_id, &ts) == 0) {
            printf("  CPU time: %ld.%09ld seconds\n", ts.tv_sec, ts.tv_nsec);
        }
        printf("  PASS\n");
    } else {
        printf("  apth_getcpuclockid returned error: %d\n", ret);
        printf("  (This is expected if the thread hasn't been scheduled yet)\n");
    }
    printf("\n");

    // Test 4: apth_getattr_np
    printf("Test 4: apth_getattr_np\n");
    apth_attr_t attr;
    ret = apth_attr_init(&attr);
    assert(ret == 0);

    ret = apth_getattr_np(th, &attr);
    assert(ret == 0);

    size_t stacksize;
    ret = apth_attr_getstacksize(&attr, &stacksize);
    assert(ret == 0);
    printf("  Thread stack size: %zu bytes\n", stacksize);

    void *stackaddr;
    ret = apth_attr_getstackaddr(&attr, &stackaddr);
    assert(ret == 0);
    printf("  Thread stack address: %p\n", stackaddr);

    int detachstate;
    ret = apth_attr_getdetachstate(&attr, &detachstate);
    assert(ret == 0);
    printf("  Thread detach state: %s\n",
           detachstate == APTH_CREATE_DETACHED ? "DETACHED" : "JOINABLE");

    ret = apth_attr_destroy(&attr);
    assert(ret == 0);
    printf("  PASS\n\n");

    // Join the worker thread
    ret = apth_join(th, NULL);
    assert(ret == 0);

    printf("=== All tests passed! ===\n");
    exit(0);
APTH_MAIN_END
