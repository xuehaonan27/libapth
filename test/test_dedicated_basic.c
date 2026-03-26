/*
 * test_dedicated_basic.c -- Test APTH_CLASS_DEDICATED thread creation
 *
 * A dedicated thread runs on its own pthread (1:1 mapping) rather than
 * being multiplexed by a scheduler.  Verify:
 *   1. Thread runs and sets a shared variable
 *   2. apth_join returns the correct exit value
 *   3. apth_self() inside the dedicated thread returns non-NULL
 *   4. apth_equal(self, self) returns nonzero inside the dedicated thread
 */
#include "apth.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

#define N_WORKERS 2

static volatile int shared_val = 0;
static _Atomic(int) self_ok = 0;
static _Atomic(int) equal_ok = 0;

static void *
dedicated_worker(void *arg)
{
    (void)arg;

    /* Verify apth_self() returns non-NULL */
    apth_t self = apth_self();
    if (self != NULL)
        atomic_store(&self_ok, 1);

    /* Verify apth_equal(self, self) returns nonzero */
    if (apth_equal(self, self))
        atomic_store(&equal_ok, 1);

    shared_val = 42;
    return (void *)123;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char starting[] = "test_dedicated_basic: starting\n";
    write(2, starting, sizeof(starting) - 1);

    apth_t th;
    apth_attr_t attr;
    apth_attr_init(&attr);
    apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);

    int rc = apth_create(&th, &attr, dedicated_worker, NULL);
    apth_attr_destroy(&attr);
    if (rc != 0)
    {
        static char err[] = "test_dedicated_basic: FAIL (apth_create failed)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    void *retval;
    rc = apth_join(th, &retval);
    if (rc != 0)
    {
        static char err[] = "test_dedicated_basic: FAIL (apth_join failed)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    if (retval != (void *)123)
    {
        static char err[] = "test_dedicated_basic: FAIL (wrong return value)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    if (shared_val != 42)
    {
        static char err[] = "test_dedicated_basic: FAIL (shared_val != 42)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    if (!atomic_load(&self_ok))
    {
        static char err[] = "test_dedicated_basic: FAIL (apth_self() returned NULL)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    if (!atomic_load(&equal_ok))
    {
        static char err[] = "test_dedicated_basic: FAIL (apth_equal(self,self) failed)\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }

    static char ok[] = "test_dedicated_basic: PASS\n";
    write(2, ok, sizeof(ok) - 1);
    exit(0);
}
APTH_MAIN_END
