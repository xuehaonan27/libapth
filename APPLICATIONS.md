# LIBAPTH Applications

This project now includes comprehensive real-world applications to test and demonstrate LIBAPTH capabilities.

## Applications

Three substantial applications have been created in the `apps/` directory:

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

✓ Thread management (create, join, pools)
✓ Socket I/O (accept, connect, send, recv)
✓ File I/O (open, read, write, close)
✓ Synchronization primitives (mutex, cond, barrier, semaphore, rwlock)
✓ Signal handling (SIGINT, SIGTERM)
✓ Thread-specific data
✓ Concurrent request/file handling
✓ Work queue patterns
✓ Rate limiting
✓ Statistics tracking

These applications provide comprehensive testing of LIBAPTH's features and demonstrate real-world usage patterns.
