# Test & Application Reorganization Plan

## Current State

76 test files in a flat `test/` directory. Classification is by filename suffix:
- `*_apth.c` → LIBAPTH I/O tests (run with `LD_PRELOAD`)
- `*_pthread.c` → pthread baseline tests (no `LD_PRELOAD`)
- `test_tcp_*.c` → TCP multi-process tests
- `test_rdma_*.c` → RDMA tests (built with `-DAPTH_USE_RDMA`)
- Everything else → "legacy" tests (run with `LD_PRELOAD`)

No test framework: each test is standalone with ad-hoc pass/fail reporting.

## Proposed Directory Structure

```
test/
├── common/
│   └── test_harness.h          # Unified test macros (see below)
│
├── core/                       # Thread lifecycle
│   ├── test_create.c
│   ├── test_join.c
│   ├── test_cancel.c
│   ├── test_kill.c
│   ├── test_many_apth.c
│   ├── test_main_exit.c
│   ├── test_main_apth_exit.c
│   ├── test_guard_page.c
│   ├── test_new_functions.c
│   ├── test_init.c
│   ├── test_sizes.c
│   ├── test_opaque_types_apth.c
│   ├── test_all_opaque_types_apth.c
│   └── test_placeholder.c
│
├── sync/                       # Synchronization primitives
│   ├── test_mutex.c
│   ├── test_mutex_advanced.c
│   ├── test_cond.c
│   ├── test_cond_advanced.c
│   ├── test_barrier.c
│   ├── test_barrier_advanced.c
│   ├── test_sem.c
│   ├── test_sem_advanced.c
│   ├── test_rwlock.c
│   ├── test_rwlock_advanced.c
│   └── test_timed.c
│
├── sched/                      # Scheduler features
│   ├── test_work_stealing.c
│   ├── test_2_workers.c
│   ├── test_2_workers_affinity.c
│   └── test_affinity.c
│
├── io/                         # I/O hook tests
│   ├── apth/                   # LIBAPTH versions
│   │   ├── test_rw_pipe.c
│   │   ├── test_rwv_pipe.c
│   │   ├── test_send_recv.c
│   │   ├── test_sendto_recvfrom.c
│   │   ├── test_dup.c
│   │   ├── test_fd_open_close.c
│   │   ├── test_fd_contention.c
│   │   ├── test_fd_shared_cross_sched.c
│   │   ├── test_pread_pwrite64.c
│   │   ├── test_preadv_pwritev.c
│   │   ├── test_socketpair.c
│   │   ├── test_contention_rw.c
│   │   ├── test_contention_socket.c
│   │   ├── test_cs_unix.c
│   │   ├── test_pause.c
│   │   └── test_fork_wait.c
│   ├── pthread/                # Baseline comparisons
│   │   ├── test_rw_pipe.c
│   │   ├── test_rwv_pipe.c
│   │   ├── test_send_recv.c
│   │   └── ...
│   └── tcp/                    # Multi-process TCP tests
│       ├── test_tcp_server.c
│       └── test_tcp_client.c
│
├── signal/                     # Signal handling
│   ├── apth/
│   │   ├── test_signal_handler.c
│   │   ├── test_signal_mask.c
│   │   ├── test_signal_raise.c
│   │   ├── test_signal_suspend.c
│   │   ├── test_signal_wait.c
│   │   └── test_signal_cross.c
│   └── pthread/
│       ├── test_signal_handler.c
│       └── ...
│
├── rdma/                       # RDMA tests
│   ├── test_rdma_sim.c
│   └── test_rdma_real.c        # Future: real hardware test
│
└── bench/                      # Performance benchmarks
    ├── test_many_apth.c
    ├── test_many_pthread.c
    └── bench_context_switch.c   # Future: microbenchmark

apps/
├── src/                        # Application source
│   ├── http_server.c
│   ├── http_client.c
│   ├── http_server_pthread.c
│   ├── http_client_pthread.c
│   ├── dining_philosophers.c
│   ├── file_processor.c
│   ├── producer_consumer.c
│   ├── readers_writers.c
│   └── signal_demo.c
├── scripts/                    # Run scripts
│   ├── run_http_server.sh
│   ├── run_http_client.sh
│   ├── run_http_server_pthread.sh
│   ├── run_http_client_pthread.sh
│   ├── demo.sh
│   ├── test_all_apps.sh
│   └── test_sync_apps.sh
└── docs/                       # App-specific docs
    ├── README.md
    ├── QUICKSTART.md
    └── FEATURE_MATRIX.md
```

## Test Harness

Create `test/common/test_harness.h` with unified macros:

```c
#ifndef __APTH_TEST_HARNESS_H
#define __APTH_TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

static int __test_failures = 0;
static int __test_count = 0;
static const char *__current_test = NULL;

#define TEST_BEGIN(name) do { \
    __current_test = (name); \
    __test_count++; \
    fprintf(stderr, "  %s: ", (name)); \
} while(0)

#define TEST_PASS() do { \
    fprintf(stderr, "PASS\n"); \
} while(0)

#define TEST_FAIL(fmt, ...) do { \
    fprintf(stderr, "FAIL (" fmt ")\n", ##__VA_ARGS__); \
    __test_failures++; \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        TEST_FAIL("%s:%d: %s != %s (%ld != %ld)", \
            __FILE__, __LINE__, #a, #b, (long)(a), (long)(b)); \
        return; \
    } \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        TEST_FAIL("%s:%d: %s is false", __FILE__, __LINE__, #expr); \
        return; \
    } \
} while(0)

#define TEST_SUITE_RESULTS(name) do { \
    if (__test_failures == 0) \
        fprintf(stderr, "%s: ALL %d TESTS PASSED\n", (name), __test_count); \
    else \
        fprintf(stderr, "%s: %d/%d TESTS FAILED\n", (name), \
            __test_failures, __test_count); \
} while(0)

#define TEST_EXIT_CODE() (__test_failures)

#endif
```

## Makefile Changes

Replace the filename-suffix-based classification with directory-based:

```makefile
# Source discovery by directory
CORE_TEST_SRCS  := $(wildcard $(TEST_DIR)/core/*.c)
SYNC_TEST_SRCS  := $(wildcard $(TEST_DIR)/sync/*.c)
SCHED_TEST_SRCS := $(wildcard $(TEST_DIR)/sched/*.c)
IO_APTH_SRCS    := $(wildcard $(TEST_DIR)/io/apth/*.c)
IO_PTHREAD_SRCS := $(wildcard $(TEST_DIR)/io/pthread/*.c)
IO_TCP_SRCS     := $(wildcard $(TEST_DIR)/io/tcp/*.c)
SIGNAL_APTH_SRCS := $(wildcard $(TEST_DIR)/signal/apth/*.c)
SIGNAL_PTHREAD_SRCS := $(wildcard $(TEST_DIR)/signal/pthread/*.c)
RDMA_SRCS       := $(wildcard $(TEST_DIR)/rdma/*.c)
BENCH_SRCS      := $(wildcard $(TEST_DIR)/bench/*.c)

# Include path for test harness
TEST_INCLUDES := -I$(TEST_DIR)/common

# APTH tests (link libapth, run with LD_PRELOAD)
APTH_TEST_SRCS := $(CORE_TEST_SRCS) $(SYNC_TEST_SRCS) $(SCHED_TEST_SRCS) \
                  $(IO_APTH_SRCS) $(SIGNAL_APTH_SRCS)

# Grouped targets
run-core-tests: ...
run-sync-tests: ...
run-io-tests: ...
run-signal-tests: ...
run-all-tests: run-core-tests run-sync-tests run-io-tests run-signal-tests
```

## Migration Script

A script to move files:

```bash
#!/bin/bash
# migrate_tests.sh - Move tests to new directory structure
# Run from project root. DRY RUN: prints mv commands without executing.

mkdir -p test/{common,core,sync,sched,io/{apth,pthread,tcp},signal/{apth,pthread},rdma,bench}

# Core
for f in test_create test_join test_cancel test_kill test_init \
         test_guard_page test_main_exit test_main_apth_exit \
         test_sizes test_placeholder test_new_functions \
         test_opaque_types_apth test_all_opaque_types_apth; do
    [ -f "test/${f}.c" ] && echo "mv test/${f}.c test/core/"
done

# Sync
for f in test_mutex test_mutex_advanced test_cond test_cond_advanced \
         test_barrier test_barrier_advanced test_sem test_sem_advanced \
         test_rwlock test_rwlock_advanced test_timed; do
    [ -f "test/${f}.c" ] && echo "mv test/${f}.c test/sync/"
done

# Sched
for f in test_work_stealing test_2_workers test_2_workers_affinity \
         test_affinity; do
    [ -f "test/${f}.c" ] && echo "mv test/${f}.c test/sched/"
done

# IO apth (files ending in _apth.c, excluding signal)
for f in test/*_apth.c; do
    base=$(basename "$f")
    case "$base" in
        test_signal_*) echo "mv $f test/signal/apth/${base%_apth.c}.c" ;;
        *)             echo "mv $f test/io/apth/${base%_apth.c}.c" ;;
    esac
done

# IO pthread
for f in test/*_pthread.c; do
    base=$(basename "$f")
    case "$base" in
        test_signal_*) echo "mv $f test/signal/pthread/${base%_pthread.c}.c" ;;
        test_many_*)   echo "mv $f test/bench/" ;;
        *)             echo "mv $f test/io/pthread/${base%_pthread.c}.c" ;;
    esac
done

# TCP
echo "mv test/test_tcp_server.c test/io/tcp/"
echo "mv test/test_tcp_client.c test/io/tcp/"

# RDMA
echo "mv test/test_rdma_sim.c test/rdma/"

# Bench
echo "mv test/test_many_apth.c test/bench/"
```

## Recommendation

The reorganization is valuable but is a **separate task** from feature
development.  It should be done as a dedicated commit that:
1. Creates the directory structure
2. Moves files (using `git mv` for history preservation)
3. Updates the Makefile
4. Verifies all tests still build and pass

Do NOT combine it with feature changes — it will make the diff unreadable.
