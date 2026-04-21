/*
 * test_class_preempt.c -- Verify class-aware preemption and IO_BOUND priority
 *
 * Setup: 1 worker, 1 CPU_BOUND spinner + 1 IO_BOUND thread that sleeps
 * then increments a counter.
 *
 * Expected: The CPU_BOUND thread gets preempted, allowing the IO_BOUND
 * thread to run and increment the counter.  Without class-aware preemption,
 * the IO_BOUND thread would starve.
 */
#include "apth.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

static _Atomic(int) io_counter = 0;
static _Atomic(int) cpu_counter = 0;

/* Each thread runs for a fixed number of iterations then exits */
#define ITERATIONS 50

static void *
cpu_spinner(void *arg)
{
    (void)arg;
    for (int n = 0; n < ITERATIONS; n++)
    {
        atomic_fetch_add(&cpu_counter, 1);
        for (volatile int i = 0; i < 100000; i++)
            ;
    }
    return NULL;
}

static void *
io_worker(void *arg)
{
    (void)arg;
    for (int n = 0; n < ITERATIONS; n++)
    {
        atomic_fetch_add(&io_counter, 1);
        apth_yield();
    }
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = 1;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc; (void)argv;

    apth_t cpu_th, io_th;
    apth_attr_t cpu_attr, io_attr;

    apth_attr_init(&cpu_attr);
    apth_attr_setclass_np(&cpu_attr, APTH_CLASS_CPU_BOUND);

    apth_attr_init(&io_attr);
    apth_attr_setclass_np(&io_attr, APTH_CLASS_IO_BOUND);

    apth_create(&cpu_th, &cpu_attr, cpu_spinner, NULL);
    apth_create(&io_th, &io_attr, io_worker, NULL);

    apth_attr_destroy(&cpu_attr);
    apth_attr_destroy(&io_attr);

    apth_join(cpu_th, NULL);
    apth_join(io_th, NULL);

    int io_val = atomic_load(&io_counter);
    int cpu_val = atomic_load(&cpu_counter);

    if (io_val == ITERATIONS && cpu_val == ITERATIONS)
    {
        static char ok[] = "test_class_preempt: PASS\n";
        write(2, ok, sizeof(ok) - 1);
        exit(0);
    }
    else
    {
        static char err[] = "test_class_preempt: FAIL\n";
        write(2, err, sizeof(err) - 1);
        exit(1);
    }
}
APTH_MAIN_END
