// Test guard page mechanism - should trigger SIGSEGV on stack overflow

#include "apth.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>

#define NULL ((void *)0)

static volatile int sigsegv_caught = 0;
static jmp_buf jump_buffer;

void sigsegv_handler(int sig)
{
    (void)sig;
    sigsegv_caught = 1;
    write(2, "\nSIGSEGV caught - guard page working!\n", 38);
    longjmp(jump_buffer, 1);
}

// Non-tail-recursive function to cause stack overflow
// Use volatile to prevent optimization
void overflow_stack(volatile int depth)
{
    volatile char buffer[4096]; // Allocate 4KB on stack each call

    // Touch the memory to ensure it's actually allocated
    for (int i = 0; i < 4096; i += 256)
    {
        buffer[i] = (char)(depth & 0xFF);
    }

    if (depth % 100 == 0)
    {
        char msg[64];
        int len = snprintf(msg, sizeof(msg), "Depth: %d\n", depth);
        write(2, msg, len);
    }

    // Prevent tail call optimization by using the buffer
    if (buffer[0] == 0xFF)
    {
        write(2, "Impossible\n", 11);
    }

    overflow_stack(depth + 1); // Recurse indefinitely

    // Use buffer again to prevent optimization
    buffer[0] = 0;
}

void *overflow_thread(void *arg)
{
    (void)arg;
    write(2, "Thread starting - will overflow stack...\n", 41);

    if (setjmp(jump_buffer) == 0)
    {
        overflow_stack(0);
    }
    else
    {
        write(2, "Returned from longjmp after SIGSEGV\n", 36);
    }

    return NULL;
}

APTH_CONFIG(cfg, cfg->workers = 1;)

APTH_MAIN_BEGIN(argc, argv)
{
    (void)argc;
    (void)argv;

    // Install SIGSEGV handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigsegv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGSEGV, &sa, NULL) != 0)
    {
        write(2, "Failed to install SIGSEGV handler\n", 34);
        exit(1);
    }

    write(2, "Testing guard page mechanism...\n", 32);
    write(2, "Creating thread that will overflow its stack\n", 45);

    apth_t th;

    // Create thread with default attributes (includes guard page)
    if (apth_create(&th, NULL, overflow_thread, NULL) != 0)
    {
        write(2, "Failed to create thread\n", 24);
        exit(1);
    }

    void *result;
    apth_join(th, &result);

    // Check if SIGSEGV was caught
    if (sigsegv_caught)
    {
        write(2, "SUCCESS: Guard page triggered SIGSEGV as expected!\n", 51);
        exit(0);
    }
    else
    {
        write(2, "ERROR: Guard page did not trigger SIGSEGV!\n", 43);
        exit(1);
    }
}
APTH_MAIN_END
