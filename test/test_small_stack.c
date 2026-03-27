// Test that a thread with a small (64 KiB) stack can run moderate recursion.
//
// This exercises the Item 6 change: check_stacksize_attr now accepts any
// stack size >= APTH_MIN_STACK (16 KiB) rather than >= APTH_STACK_SIZE_DEFAULT
// (2 MiB).  A 64 KiB stack should be accepted and functional.

#include "apth.h"
#include <stdlib.h>
#include <unistd.h>

#define SMALL_STACK_SIZE 65536 // 64 KiB

// Recursive Fibonacci — depth 20 uses moderate stack (roughly 20 frames).
static int fib(int n)
{
    if (n <= 1)
        return n;
    return fib(n - 1) + fib(n - 2);
}

static void *fib_thread(void *arg)
{
    int depth = (int)(long)arg;
    int result = fib(depth);
    return (void *)(long)result;
}

APTH_CONFIG(cfg,
            cfg->workers = 2;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    // First verify that apth_attr_setstacksize accepts 64 KiB
    apth_attr_t attr;
    apth_attr_init(&attr);
    int rc = apth_attr_setstacksize(&attr, SMALL_STACK_SIZE);
    if (rc != 0)
    {
        static const char msg[] = "FAIL: apth_attr_setstacksize rejected 64 KiB\n";
        write(2, msg, sizeof(msg) - 1);
        exit(1);
    }

    // Verify the size was actually stored
    size_t got_size;
    apth_attr_getstacksize(&attr, &got_size);
    if (got_size != SMALL_STACK_SIZE)
    {
        static const char msg[] = "FAIL: getstacksize returned wrong value\n";
        write(2, msg, sizeof(msg) - 1);
        exit(1);
    }

    // Create a thread with the small stack and run fibonacci(20)
    apth_t th;
    rc = apth_create(&th, &attr, fib_thread, (void *)(long)20);
    if (rc != 0)
    {
        static const char msg[] = "FAIL: apth_create with 64 KiB stack failed\n";
        write(2, msg, sizeof(msg) - 1);
        exit(1);
    }

    void *retval;
    apth_join(th, &retval);
    apth_attr_destroy(&attr);

    int result = (int)(long)retval;
    // fib(20) == 6765
    if (result == 6765)
    {
        static const char msg[] = "PASS\n";
        write(2, msg, sizeof(msg) - 1);
    }
    else
    {
        static const char msg[] = "FAIL: fib(20) returned wrong result\n";
        write(2, msg, sizeof(msg) - 1);
        exit(1);
    }
}
APTH_MAIN_END
