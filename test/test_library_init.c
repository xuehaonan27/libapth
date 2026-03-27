// Test library-mode initialization (apth_init_library).
//
// This test does NOT use APTH_CONFIG or APTH_MAIN_BEGIN/END.
// It has a plain int main() that calls apth_init_library(), creates
// several apth threads, joins them, and verifies correctness.

#include "apth.h"
#include <stdlib.h>
#include <unistd.h>

static apth_mutex_t mtx;
static volatile int counter = 0;

#define N_THREADS 3
#define ITERS_PER_THREAD 1000

static void *worker(void *arg)
{
    for (int i = 0; i < ITERS_PER_THREAD; i++)
    {
        apth_mutex_lock(&mtx);
        counter++;
        apth_mutex_unlock(&mtx);
    }
    return arg;
}

int main(void)
{
    if (apth_init_library(2) != 0)
    {
        static const char msg[] = "FAIL: apth_init_library failed\n";
        write(2, msg, sizeof(msg) - 1);
        return 1;
    }

    apth_mutex_init(&mtx, NULL);

    apth_t tids[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
    {
        int rc = apth_create(&tids[i], NULL, worker, (void *)(long)i);
        if (rc != 0)
        {
            static const char msg[] = "FAIL: apth_create failed\n";
            write(2, msg, sizeof(msg) - 1);
            return 1;
        }
    }

    for (int i = 0; i < N_THREADS; i++)
    {
        void *retval;
        int jrc = apth_join(tids[i], &retval);
        if (jrc != 0)
        {
            static const char msg[] = "FAIL: apth_join returned error\n";
            write(2, msg, sizeof(msg) - 1);
            return 1;
        }
        if ((long)retval != (long)i)
        {
            static const char msg[] = "FAIL: wrong retval from join\n";
            write(2, msg, sizeof(msg) - 1);
            return 1;
        }
    }

    int expected = N_THREADS * ITERS_PER_THREAD;
    if (counter == expected)
    {
        static const char msg[] = "PASS\n";
        write(2, msg, sizeof(msg) - 1);
    }
    else
    {
        static const char msg[] = "FAIL: counter mismatch\n";
        write(2, msg, sizeof(msg) - 1);
        return 1;
    }

    // Use _exit to bypass apth_drop() shutdown — library-mode graceful
    // shutdown is tested separately in test_shutdown.c (Phase 3, Item 10).
    // The core library-mode functionality (init + create + join) is verified.
    _exit(0);
    return 0;
}
