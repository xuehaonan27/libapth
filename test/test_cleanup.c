/*
 * test_cleanup.c — Test apth_cleanup_push / apth_cleanup_pop
 *
 * 1. push+pop(1): handler runs
 * 2. push+pop(0): handler does NOT run
 * 3. push + cancel: handler runs on cancellation
 */
#include "apth.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

static _Atomic(int) handler_a_ran = 0;
static _Atomic(int) handler_b_ran = 0;
static _Atomic(int) handler_cancel_ran = 0;

static void handler_a(void *arg) {
    (void)arg;
    atomic_store(&handler_a_ran, 1);
}

static void handler_b(void *arg) {
    (void)arg;
    atomic_store(&handler_b_ran, 1);
}

static void handler_cancel(void *arg) {
    (void)arg;
    atomic_store(&handler_cancel_ran, 1);
}

/* Thread that pushes+pops cleanup handlers */
static void *pop_test_worker(void *arg)
{
    (void)arg;

    /* push A, pop(1) — should execute */
    apth_cleanup_push(handler_a, NULL);
    apth_cleanup_pop(1);

    /* push B, pop(0) — should NOT execute */
    apth_cleanup_push(handler_b, NULL);
    apth_cleanup_pop(0);

    return NULL;
}

/* Thread that gets cancelled with a cleanup handler */
static void *cancel_test_worker(void *arg)
{
    (void)arg;

    apth_cleanup_push(handler_cancel, NULL);

    /* Enable cancellation */
    apth_setcancelstate(APTH_CANCEL_ENABLE, NULL);
    apth_setcanceltype(APTH_CANCEL_DEFERRED, NULL);

    /* Block until cancelled */
    for (;;)
    {
        apth_testcancel();
        apth_yield();
    }

    /* Not reached, but required by push/pop pairing */
    apth_cleanup_pop(0);
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = 2;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    /* Test 1: push+pop */
    apth_t th1;
    apth_create(&th1, NULL, pop_test_worker, NULL);
    apth_join(th1, NULL);

    if (!atomic_load(&handler_a_ran))
    {
        write(2, "test_cleanup: FAIL (pop(1) did not run handler)\n", 48);
        exit(1);
    }
    if (atomic_load(&handler_b_ran))
    {
        write(2, "test_cleanup: FAIL (pop(0) ran handler)\n", 39);
        exit(1);
    }

    /* Test 2: cancel with cleanup */
    apth_t th2;
    apth_create(&th2, NULL, cancel_test_worker, NULL);
    usleep(10000); /* Let it start */
    apth_cancel(th2);

    void *retval;
    apth_join(th2, &retval);

    if (retval != APTH_CANCELED)
    {
        write(2, "test_cleanup: FAIL (cancel did not return APTH_CANCELED)\n", 56);
        exit(1);
    }
    if (!atomic_load(&handler_cancel_ran))
    {
        write(2, "test_cleanup: FAIL (cancel cleanup handler did not run)\n", 55);
        exit(1);
    }

    write(2, "test_cleanup: PASS\n", 19);
    exit(0);
}
APTH_MAIN_END
