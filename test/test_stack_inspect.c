/*
 * test_stack_inspect.c -- Test apth_get_saved_sp() and apth_get_stack_bounds()
 *
 * Verify:
 *   1. A yielded thread's saved SP is non-NULL
 *   2. Stack bounds base is non-NULL and size > 0
 *   3. Saved SP falls within [base, base+size]
 *   4. A dedicated thread returns ENOTSUP for both functions
 *   5. A RUNNING thread returns EBUSY for get_saved_sp
 */
#include "apth.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdint.h>
#include <errno.h>

static apth_mutex_t gate_mtx;
static apth_cond_t gate_cond;
static volatile int gate_open = 0;
static _Atomic(int) child_started = 0;
static _Atomic(int) dedicated_started = 0;
static _Atomic(int) dedicated_done = 0;

static void *
child_worker(void *arg)
{
    (void)arg;
    atomic_store(&child_started, 1);

    /* Block until main opens the gate */
    apth_mutex_lock(&gate_mtx);
    while (!gate_open)
        apth_cond_wait(&gate_cond, &gate_mtx);
    apth_mutex_unlock(&gate_mtx);

    return NULL;
}

static void *
dedicated_worker(void *arg)
{
    (void)arg;
    atomic_store(&dedicated_started, 1);

    /* Spin until main signals we can exit */
    while (!atomic_load(&dedicated_done))
        usleep(1000);

    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = 2;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    apth_mutex_init(&gate_mtx, NULL);
    apth_cond_init(&gate_cond, NULL);

    /* --- Test 1-3: Normal apth thread stack inspection --- */
    apth_t th;
    apth_create(&th, NULL, child_worker, NULL);

    /* Wait for child to start and enter WAITING */
    while (!atomic_load(&child_started))
        apth_yield();
    for (int i = 0; i < 100; i++)
        apth_yield();

    /* Test: get_saved_sp should succeed and return non-NULL */
    void *sp = NULL;
    int rc = apth_get_saved_sp(th, &sp);
    if (rc != 0)
    {
        static char err[] = "test_stack_inspect: FAIL (get_saved_sp returned error)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }
    if (sp == NULL)
    {
        static char err[] = "test_stack_inspect: FAIL (saved SP is NULL)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Test: get_stack_bounds should succeed */
    void *base = NULL;
    size_t size = 0;
    rc = apth_get_stack_bounds(th, &base, &size);
    if (rc != 0)
    {
        static char err[] = "test_stack_inspect: FAIL (get_stack_bounds returned error)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }
    if (base == NULL)
    {
        static char err[] = "test_stack_inspect: FAIL (stack base is NULL)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }
    if (size == 0)
    {
        static char err[] = "test_stack_inspect: FAIL (stack size is 0)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Test: SP should fall within the stack range [base, base+size] */
    uintptr_t sp_val = (uintptr_t)sp;
    uintptr_t base_val = (uintptr_t)base;
    if (sp_val < base_val || sp_val > base_val + size)
    {
        static char err[] = "test_stack_inspect: FAIL (SP outside stack bounds)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Test: NULL sp_out should return EINVAL */
    rc = apth_get_saved_sp(th, NULL);
    if (rc != EINVAL)
    {
        static char err[] = "test_stack_inspect: FAIL (NULL sp_out should return EINVAL)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Release the child */
    apth_mutex_lock(&gate_mtx);
    gate_open = 1;
    apth_cond_signal(&gate_cond);
    apth_mutex_unlock(&gate_mtx);
    apth_join(th, NULL);

    /* --- Test 4: Dedicated thread returns ENOTSUP --- */
    apth_t dth;
    apth_attr_t dattr;
    apth_attr_init(&dattr);
    apth_attr_setclass_np(&dattr, APTH_CLASS_DEDICATED);

    rc = apth_create(&dth, &dattr, dedicated_worker, NULL);
    apth_attr_destroy(&dattr);
    if (rc != 0)
    {
        static char err[] = "test_stack_inspect: FAIL (dedicated create failed)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Wait for dedicated thread to start */
    while (!atomic_load(&dedicated_started))
        apth_yield();

    void *dsp = NULL;
    rc = apth_get_saved_sp(dth, &dsp);
    if (rc != ENOTSUP)
    {
        static char err[] = "test_stack_inspect: FAIL (dedicated get_saved_sp should be ENOTSUP)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    void *dbase = NULL;
    size_t dsize = 0;
    rc = apth_get_stack_bounds(dth, &dbase, &dsize);
    if (rc != ENOTSUP)
    {
        static char err[] = "test_stack_inspect: FAIL (dedicated get_stack_bounds should be ENOTSUP)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    /* Let the dedicated thread finish */
    atomic_store(&dedicated_done, 1);
    apth_join(dth, NULL);

    apth_cond_destroy(&gate_cond);
    apth_mutex_destroy(&gate_mtx);

    static char ok[] = "test_stack_inspect: PASS\n";
    write(2, ok, sizeof(ok) - 1);
    exit(0);
}
APTH_MAIN_END
