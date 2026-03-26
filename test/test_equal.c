/*
 * test_equal.c — Test apth_equal()
 *
 * Verify: same thread returns nonzero, different threads return zero.
 */
#include "apth.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

static apth_t main_thread_id;
static apth_t child_thread_id;
static _Atomic(int) child_self_ok = 0;
static _Atomic(int) child_neq_main_ok = 0;

static void *child_worker(void *arg)
{
    (void)arg;
    apth_t self = apth_self();
    child_thread_id = self;

    /* Self-equality: apth_equal(self, self) should be nonzero */
    if (apth_equal(self, self))
        atomic_store(&child_self_ok, 1);

    /* Inequality with main thread */
    if (!apth_equal(self, main_thread_id))
        atomic_store(&child_neq_main_ok, 1);

    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = 2;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    main_thread_id = apth_self();

    /* Self-equality for main thread */
    if (!apth_equal(main_thread_id, main_thread_id))
    {
        write(2, "test_equal: FAIL (main self-equality)\n", 37);
        exit(1);
    }

    apth_t th;
    apth_create(&th, NULL, child_worker, NULL);
    apth_join(th, NULL);

    /* Check child's self-equality */
    if (!atomic_load(&child_self_ok))
    {
        write(2, "test_equal: FAIL (child self-equality)\n", 38);
        exit(1);
    }

    /* Check child != main */
    if (!atomic_load(&child_neq_main_ok))
    {
        write(2, "test_equal: FAIL (child != main)\n", 32);
        exit(1);
    }

    /* Main != child */
    if (apth_equal(main_thread_id, child_thread_id))
    {
        write(2, "test_equal: FAIL (main == child)\n", 32);
        exit(1);
    }

    write(2, "test_equal: PASS\n", 17);
    exit(0);
}
APTH_MAIN_END
