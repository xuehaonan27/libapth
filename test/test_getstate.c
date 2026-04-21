/*
 * test_getstate.c -- Test apth_getstate() thread state inspection
 *
 * Verify:
 *   1. A thread blocked on a mutex is in APTH_THREAD_STATE_WAITING
 *   2. After join, the thread is in APTH_THREAD_STATE_TERMINATED
 *   3. Invalid thread returns -1
 */
#include "apth.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

static apth_mutex_t gate_mtx;
static apth_cond_t gate_cond;
static volatile int gate_open = 0;
static _Atomic(int) child_started = 0;

static void *
child_worker(void *arg)
{
    (void)arg;
    atomic_store(&child_started, 1);

    /* Block on the condition variable until main opens the gate */
    apth_mutex_lock(&gate_mtx);
    while (!gate_open)
        apth_cond_wait(&gate_cond, &gate_mtx);
    apth_mutex_unlock(&gate_mtx);

    return (void *)42;
}

APTH_CONFIG(cfg, cfg->workers = 2;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    apth_mutex_init(&gate_mtx, NULL);
    apth_cond_init(&gate_cond, NULL);

    apth_t th;
    apth_create(&th, NULL, child_worker, NULL);

    /* Wait for child to start and enter the cond_wait (WAITING state) */
    while (!atomic_load(&child_started))
        apth_yield();

    /* Give the child time to actually block on the cond_wait */
    for (int i = 0; i < 100; i++)
        apth_yield();

    /* Check: thread should be WAITING */
    int state = apth_getstate(th);
    if (state != APTH_THREAD_STATE_WAITING)
    {
        static char err[] = "test_getstate: FAIL (expected WAITING)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Open the gate so the child can proceed */
    apth_mutex_lock(&gate_mtx);
    gate_open = 1;
    apth_cond_signal(&gate_cond);
    apth_mutex_unlock(&gate_mtx);

    void *retval;
    apth_join(th, &retval);

    if (retval != (void *)42)
    {
        static char err[] = "test_getstate: FAIL (wrong return value)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* After join, the TCB is freed — getstate should return -1 (ESRCH) */
    state = apth_getstate(th);
    if (state != -1)
    {
        static char err[] = "test_getstate: FAIL (expected -1 after join)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Check: NULL thread returns -1 */
    state = apth_getstate(NULL);
    if (state != -1)
    {
        static char err[] = "test_getstate: FAIL (NULL should return -1)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    apth_cond_destroy(&gate_cond);
    apth_mutex_destroy(&gate_mtx);

    static char ok[] = "test_getstate: PASS\n";
    write(2, ok, sizeof(ok) - 1);
    exit(0);
}
APTH_MAIN_END
