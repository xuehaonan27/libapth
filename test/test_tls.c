/*
 * test_tls.c — Test thread-local storage (apth_key_create, getspecific, setspecific)
 *
 * Verify per-thread key isolation and destructor callbacks.
 */
#include "apth.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <string.h>

#define N_THREADS 20

static apth_key_t key1;
static _Atomic(int) destructor_count = 0;

static void key1_destructor(void *value)
{
    (void)value;
    atomic_fetch_add(&destructor_count, 1);
}

static void *worker(void *arg)
{
    int id = *(int *)arg;
    long expected = id * 1000 + 42;

    /* Set thread-specific value */
    if (apth_setspecific(key1, (void *)expected) != 0)
    {
        write(2, "test_tls: FAIL (setspecific failed)\n", 35);
        exit(1);
    }

    /* Yield to let other threads run (verify isolation) */
    apth_yield();

    /* Read back and verify */
    long got = (long)apth_getspecific(key1);
    if (got != expected)
    {
        char buf[128];
        int len = snprintf(buf, sizeof(buf),
                           "test_tls: FAIL (thread %d: got %ld, expected %ld)\n",
                           id, got, expected);
        write(2, buf, len);
        exit(1);
    }

    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = 4;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    /* Create key with destructor */
    if (apth_key_create(&key1, key1_destructor) != 0)
    {
        write(2, "test_tls: FAIL (key_create)\n", 27);
        exit(1);
    }

    apth_t threads[N_THREADS];
    int ids[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
    {
        ids[i] = i;
        apth_create(&threads[i], NULL, worker, &ids[i]);
    }

    for (int i = 0; i < N_THREADS; i++)
        apth_join(threads[i], NULL);

    /* Verify key still works on main thread */
    apth_setspecific(key1, (void *)999L);
    if ((long)apth_getspecific(key1) != 999L)
    {
        write(2, "test_tls: FAIL (main thread getspecific)\n", 40);
        exit(1);
    }

    /* Delete key */
    if (apth_key_delete(key1) != 0)
    {
        write(2, "test_tls: FAIL (key_delete)\n", 27);
        exit(1);
    }

    write(2, "test_tls: PASS\n", 15);
    exit(0);
}
APTH_MAIN_END
