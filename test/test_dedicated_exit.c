/*
 * test_dedicated_exit.c -- Test apth_exit() and cleanup handlers
 *                          for dedicated threads
 *
 * Verify:
 *   1. A dedicated thread that calls apth_exit((void*)999) delivers
 *      the correct exit value via apth_join.
 *   2. A cleanup handler pushed before apth_exit is executed.
 */
#include "apth.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

#define N_WORKERS 2

static _Atomic(int) cleanup_ran = 0;

static void
cleanup_handler(void *arg)
{
    (void)arg;
    atomic_store(&cleanup_ran, 1);
}

/* Thread that calls apth_exit with an explicit value */
static void *
exit_worker(void *arg)
{
    (void)arg;
    apth_exit((void *)999);
    /* Not reached */
    return NULL;
}

/* Thread that pushes a cleanup handler then calls apth_exit */
static void *
cleanup_exit_worker(void *arg)
{
    (void)arg;
    apth_cleanup_push(cleanup_handler, NULL);
    apth_exit((void *)777);
    /* Not reached, but required by push/pop pairing */
    apth_cleanup_pop(0);
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char starting[] = "test_dedicated_exit: starting\n";
    write(2, starting, sizeof(starting) - 1);

    /* Test 1: apth_exit with return value */
    {
        apth_t th;
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
        int rc = apth_create(&th, &attr, exit_worker, NULL);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] = "test_dedicated_exit: FAIL (create exit_worker failed)\n";
            write(2, err, sizeof(err) - 1);
            exit(1);
        }

        void *retval;
        apth_join(th, &retval);
        if (retval != (void *)999)
        {
            static char err[] = "test_dedicated_exit: FAIL (retval != 999)\n";
            write(2, err, sizeof(err) - 1);
            exit(1);
        }
    }

    /* Test 2: cleanup handler runs on apth_exit */
    {
        apth_t th;
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
        int rc = apth_create(&th, &attr, cleanup_exit_worker, NULL);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] = "test_dedicated_exit: FAIL (create cleanup_exit_worker failed)\n";
            write(2, err, sizeof(err) - 1);
            exit(1);
        }

        void *retval;
        apth_join(th, &retval);
        if (retval != (void *)777)
        {
            static char err[] = "test_dedicated_exit: FAIL (cleanup retval != 777)\n";
            write(2, err, sizeof(err) - 1);
            exit(1);
        }

        if (!atomic_load(&cleanup_ran))
        {
            static char err[] = "test_dedicated_exit: FAIL (cleanup handler did not run)\n";
            write(2, err, sizeof(err) - 1);
            exit(1);
        }
    }

    static char ok[] = "test_dedicated_exit: PASS\n";
    write(2, ok, sizeof(ok) - 1);
    exit(0);
}
APTH_MAIN_END
