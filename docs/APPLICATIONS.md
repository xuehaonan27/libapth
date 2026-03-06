# LIBAPTH Applications

This project now includes comprehensive real-world applications to test and demonstrate LIBAPTH capabilities.

## Applications

Seven substantial applications have been created in the `apps/` directory:

### I/O-Focused Applications

1. **HTTP Server** (`http_server.c`) - Multi-threaded web server
   - Thread pool with work queue
   - Socket I/O operations
   - File serving
   - Signal handling for graceful shutdown

2. **HTTP Client** (`http_client.c`) - Load testing client
   - Concurrent connections
   - Barrier synchronization
   - Performance measurement

3. **File Processor** (`file_processor.c`) - Concurrent file analysis
   - File I/O operations
   - Semaphore rate limiting
   - Text analysis and reporting

### Synchronization-Focused Applications

4. **Producer-Consumer** (`producer_consumer.c`) - Classic bounded buffer problem
   - Multiple producers and consumers
   - Mutex and condition variables
   - Thread-specific data
   - Once-only initialization

5. **Readers-Writers** (`readers_writers.c`) - Concurrent read/exclusive write
   - Read-write locks (rwlock)
   - Multiple concurrent readers
   - Exclusive writer access
   - Barrier synchronization

6. **Dining Philosophers** (`dining_philosophers.c`) - Resource allocation problem
   - Deadlock avoidance
   - Mutex for fork synchronization
   - Semaphore for limiting diners
   - Resource ordering strategy

7. **Signal Demo** (`signal_demo.c`) - Signal handling and cancellation
   - Signal handling (SIGUSR1, SIGUSR2, SIGINT, SIGTERM)
   - Thread cancellation
   - Cleanup handlers
   - Signal masks per thread

## Quick Start

```bash
# Build applications
make apps

# Run HTTP server
./apps/run_http_server.sh 8080 4 ./www

# In another terminal, run load test
./apps/run_http_client.sh localhost 8080 10 100

# Run file processor
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/file_processor ./input ./output 4

# Or run automated demo
./apps/demo.sh

# Or run comprehensive test suite
./apps/test_all_apps.sh
```

## Documentation

- `apps/README.md` - Detailed documentation for each application
- `apps/QUICKSTART.md` - Quick start guide with examples
- Sample files provided in `www/` and `input/` directories

## What These Applications Test

✓ **Thread management** (create, join, pools, cancellation)
✓ **Socket I/O** (accept, connect, send, recv)
✓ **File I/O** (open, read, write, close)
✓ **Synchronization primitives:**
  - Mutex (all applications)
  - Condition variables (HTTP Server, File Processor, Producer-Consumer)
  - Barrier (HTTP Client, Readers-Writers)
  - Semaphore (File Processor, Dining Philosophers)
  - RWLock (HTTP Server, Readers-Writers)
✓ **Signal handling** (SIGUSR1, SIGUSR2, SIGINT, SIGTERM)
✓ **Thread-specific data** (File Processor, Producer-Consumer)
✓ **Once-only initialization** (Producer-Consumer)
✓ **Cleanup handlers** (Signal Demo)
✓ **Signal masks** (Signal Demo)
✓ **Concurrent request/file handling**
✓ **Work queue patterns**
✓ **Rate limiting**
✓ **Statistics tracking**
✓ **Classic concurrency problems** (Producer-Consumer, Readers-Writers, Dining Philosophers)

These applications provide comprehensive testing of LIBAPTH's features and demonstrate real-world usage patterns.
