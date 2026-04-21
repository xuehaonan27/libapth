/*
 * test_preempt.c — Verify that signal-based preemption prevents a
 * CPU-bound M:N thread from starving other threads.
 *
 * Creates two M:N threads on a single worker:
 *   - "spinner": tight CPU loop, never calls any LIBAPTH primitive
 *   - "counter": increments a volatile counter, yields voluntarily
 *
 * Without preemption, the spinner monopolises the worker and counter
 * never runs.  With APTH_PREEMPT_SIGNAL, the spinner is preempted
 * every quantum (10ms) so counter makes progress.
 *
 * PASS: counter > 0 after 500ms.
 */
#include <apth.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int counter_value = 0;
static volatile int stop_flag = 0;

static void *spinner_fn(void *arg)
{
    (void)arg;
    while (!stop_flag)
    {
        // Pure CPU burn — no LIBAPTH calls, no I/O.
        volatile int x = 0;
        for (int i = 0; i < 100000; i++)
            x += i;
        (void)x;
    }
    return NULL;
}

static void *counter_fn(void *arg)
{
    (void)arg;
    while (!stop_flag)
    {
        counter_value++;
        apth_yield();
    }
    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = 1;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    apth_t spinner, counter;
    apth_attr_t attr;

    apth_attr_init(&attr);
    apth_attr_setname_np(&attr, "spinner");
    apth_create(&spinner, &attr, spinner_fn, NULL);

    apth_attr_setname_np(&attr, "counter");
    apth_create(&counter, &attr, counter_fn, NULL);
    apth_attr_destroy(&attr);

    // Let them run for 500ms.
    struct timespec ts = {0, 500000000L};
    nanosleep(&ts, NULL);

    stop_flag = 1;

    apth_join(spinner, NULL);
    apth_join(counter, NULL);

    if (counter_value > 0)
    {
        printf("[PASS] test_preempt: counter=%d (preemption working)\n",
               counter_value);
        return 0;
    }
    else
    {
        printf("[FAIL] test_preempt: counter=%d (spinner starved counter)\n",
               counter_value);
        return 1;
    }
}
APTH_MAIN_END
