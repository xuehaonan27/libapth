#include "apth.h"
#include "utils/apth_string.h"
#include <stdlib.h>
#include <stdint.h>

/*
 * Test: complex mixed scenario with dedicated and regular apth threads.
 *
 * 2 dedicated threads + 4 regular apths share a mutex, cond, and counter.
 *
 * - Dedicated thread 0: 500 lock/increment/unlock cycles.
 *   Also signals the cond every 100 increments (5 signals total).
 * - Dedicated thread 1: 500 lock/increment/unlock cycles (plain).
 * - Regular apths 0-2: each does 500 lock/increment/unlock cycles (plain).
 * - Regular apth 3: waits on cond and counts signals received.
 *   It also does 500 lock/increment/unlock cycles between waits.
 *
 * Expected final counter = (2 + 4) * 500 = 3000.
 */

#define N_DEDICATED     2
#define N_REGULAR       4
#define N_TOTAL         (N_DEDICATED + N_REGULAR)
#define INCREMENTS_EACH 500
#define SIGNAL_EVERY    100
#define N_WORKERS       2

static apth_mutex_t mtx;
static apth_cond_t  cond;
static volatile int counter = 0;

/* Signal accounting: the signaler sets signal_pending before signaling,
 * the waiter consumes it. */
static volatile int signal_pending = 0;
static volatile int signals_sent   = 0;
static volatile int signals_recvd  = 0;
static volatile int done_signaling = 0;

/*
 * Dedicated thread 0: increment + signal cond every SIGNAL_EVERY increments.
 */
static void *
dedicated_signaler(void *arg)
{
    (void)arg;

    for (int i = 0; i < INCREMENTS_EACH; i++)
    {
        apth_mutex_lock(&mtx);
        counter++;

        if ((i + 1) % SIGNAL_EVERY == 0)
        {
            signal_pending = 1;
            signals_sent++;
            apth_cond_signal(&cond);
        }

        apth_mutex_unlock(&mtx);
    }

    /* Mark signaling done so waiter knows to stop */
    apth_mutex_lock(&mtx);
    done_signaling = 1;
    apth_cond_signal(&cond); /* wake waiter one last time */
    apth_mutex_unlock(&mtx);

    return NULL;
}

/*
 * Dedicated thread 1 / regular apths 0-2: plain increment.
 */
static void *
plain_incrementer(void *arg)
{
    (void)arg;

    for (int i = 0; i < INCREMENTS_EACH; i++)
    {
        apth_mutex_lock(&mtx);
        counter++;
        apth_mutex_unlock(&mtx);
    }

    return NULL;
}

/*
 * Regular apth 3: waits for signals on cond, also does its own increments.
 *
 * Strategy: do increments in small batches, checking for pending signals
 * in between.  This ensures the thread does both waiting and incrementing.
 */
static void *
signal_waiter(void *arg)
{
    (void)arg;

    int my_increments = 0;
    int batch = 50; /* increment 50 at a time, then check for signals */

    while (my_increments < INCREMENTS_EACH || (!done_signaling))
    {
        /* Do a batch of increments if we still have work */
        if (my_increments < INCREMENTS_EACH)
        {
            int todo = INCREMENTS_EACH - my_increments;
            if (todo > batch)
                todo = batch;

            for (int i = 0; i < todo; i++)
            {
                apth_mutex_lock(&mtx);
                counter++;
                apth_mutex_unlock(&mtx);
                my_increments++;
            }
        }

        /* Check for and consume any pending signal */
        apth_mutex_lock(&mtx);
        while (!signal_pending && !done_signaling)
            apth_cond_wait(&cond, &mtx);

        if (signal_pending)
        {
            signal_pending = 0;
            signals_recvd++;
        }
        apth_mutex_unlock(&mtx);

        /* If signaling is done and we have finished our increments, exit */
        if (done_signaling && my_increments >= INCREMENTS_EACH)
            break;
    }

    return NULL;
}

APTH_CONFIG(cfg,
            cfg->workers = N_WORKERS;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    static char msg[] = "test_dedicated_mixed: starting\n";
    write(2, msg, sizeof(msg) - 1);

    apth_mutex_init(&mtx, NULL);
    apth_cond_init(&cond, NULL);

    apth_t tids[N_TOTAL];
    int idx = 0;
    int rc;
    int failures = 0;

    /* Dedicated thread 0: signaler + incrementer */
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
        rc = apth_create(&tids[idx], &attr, dedicated_signaler, NULL);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] =
                "test_dedicated_mixed: dedicated[0] create failed\n";
            write(2, err, sizeof(err) - 1);
            exit(EXIT_FAILURE);
        }
        idx++;
    }

    /* Dedicated thread 1: plain incrementer */
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        apth_attr_setclass_np(&attr, APTH_CLASS_DEDICATED);
        rc = apth_create(&tids[idx], &attr, plain_incrementer, NULL);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] =
                "test_dedicated_mixed: dedicated[1] create failed\n";
            write(2, err, sizeof(err) - 1);
            exit(EXIT_FAILURE);
        }
        idx++;
    }

    /* Regular apths 0-2: plain incrementers */
    for (int i = 0; i < N_REGULAR - 1; i++)
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        rc = apth_create(&tids[idx], &attr, plain_incrementer, NULL);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] =
                "test_dedicated_mixed: regular create failed\n";
            write(2, err, sizeof(err) - 1);
            exit(EXIT_FAILURE);
        }
        idx++;
    }

    /* Regular apth 3: signal waiter + incrementer */
    {
        apth_attr_t attr;
        apth_attr_init(&attr);
        apth_attr_setstacksize(&attr, 4096);
        rc = apth_create(&tids[idx], &attr, signal_waiter, NULL);
        apth_attr_destroy(&attr);
        if (rc != 0)
        {
            static char err[] =
                "test_dedicated_mixed: waiter create failed\n";
            write(2, err, sizeof(err) - 1);
            exit(EXIT_FAILURE);
        }
        idx++;
    }

    /* Join all threads */
    for (int i = 0; i < N_TOTAL; i++)
        apth_join(tids[i], NULL);

    /* Verify counter */
    int expected = N_TOTAL * INCREMENTS_EACH; /* 6 * 500 = 3000 */
    if (counter != expected)
    {
        char buf[128];
        int len = apth_snprintf(
            buf, sizeof(buf),
            "test_dedicated_mixed: FAIL counter=%d expected=%d\n",
            counter, expected);
        write(2, buf, len);
        failures++;
    }

    /* Verify signals were sent */
    int expected_signals = INCREMENTS_EACH / SIGNAL_EVERY; /* 500/100 = 5 */
    if (signals_sent != expected_signals)
    {
        char buf[128];
        int len = apth_snprintf(
            buf, sizeof(buf),
            "test_dedicated_mixed: FAIL signals_sent=%d expected=%d\n",
            signals_sent, expected_signals);
        write(2, buf, len);
        failures++;
    }

    /* The waiter should have received at least 1 signal (it may miss some
     * if signals arrive while it is doing increments, since cond_signal
     * without a waiter is a no-op). We check it received at least 1 to
     * confirm the cond path was exercised. */
    if (signals_recvd < 1)
    {
        char buf[128];
        int len = apth_snprintf(
            buf, sizeof(buf),
            "test_dedicated_mixed: FAIL signals_recvd=%d (expected >= 1)\n",
            signals_recvd);
        write(2, buf, len);
        failures++;
    }

    apth_cond_destroy(&cond);
    apth_mutex_destroy(&mtx);

    if (failures == 0)
    {
        static char ok[] = "test_dedicated_mixed: PASS\n";
        write(2, ok, sizeof(ok) - 1);
        exit(0);
    }
    else
    {
        static char fail[] = "test_dedicated_mixed: FAIL\n";
        write(2, fail, sizeof(fail) - 1);
        exit(EXIT_FAILURE);
    }
}
APTH_MAIN_END
