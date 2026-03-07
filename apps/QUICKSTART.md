# LIBAPTH Applications - Quick Start Guide

## Overview

This directory contains seven comprehensive applications that demonstrate LIBAPTH's capabilities:

### I/O-Focused
1. **HTTP Server** - Multi-threaded web server with thread pool
2. **HTTP Client** - Load testing client with concurrent connections
3. **File Processor** - Concurrent file analysis with rate limiting

### Synchronization-Focused
4. **Producer-Consumer** - Classic bounded buffer problem
5. **Readers-Writers** - Concurrent read/exclusive write problem
6. **Dining Philosophers** - Resource allocation and deadlock avoidance
7. **Signal Demo** - Signal handling and thread cancellation

## Quick Start

### Build Everything
```bash
cd /path/to/libapth
make clean
make all
make apps
```

### Run Individual Applications

#### HTTP Server
```bash
./apps/run_http_server.sh [port] [workers] [www_root]
# Example: ./apps/run_http_server.sh 8080 4 ./www
# Example: ./apps/run_http_server_pthread.sh 8080 4 ./www
```

#### HTTP Client
```bash
./apps/run_http_client.sh [host] [port] [threads] [requests]
# Example: ./apps/run_http_client.sh localhost 8080 10 100
# Example: ./apps/run_http_client_pthread.sh localhost 8080 10 100
# perf record -g -- ./apps/run_http_client.sh localhost 8080 10 1000
```

#### File Processor
```bash
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/file_processor [input_dir] [output_dir] [workers]
# Example: build/bin/file_processor ./input ./output 4
```

#### Producer-Consumer
```bash
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/producer_consumer [producers] [consumers] [buffer_size] [items]
# Example: build/bin/producer_consumer 3 2 10 20
```

#### Readers-Writers
```bash
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/readers_writers [readers] [writers] [iterations]
# Example: build/bin/readers_writers 5 2 10
```

#### Dining Philosophers
```bash
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/dining_philosophers [philosophers] [meals]
# Example: build/bin/dining_philosophers 5 10
```

#### Signal Demo
```bash
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/signal_demo [workers] [duration]
# Example: build/bin/signal_demo 4 30
```

### Run All Tests
```bash
./apps/test_all_apps.sh
```

### Run Demo
```bash
./apps/demo.sh
```

## What These Applications Test

### Thread Management
- ✓ Creating multiple apth threads
- ✓ Joining threads
- ✓ Thread pools with worker threads
- ✓ Thread-specific data

### I/O Operations
- ✓ Socket I/O: accept(), connect(), send(), recv()
- ✓ File I/O: open(), read(), write(), close()
- ✓ Directory operations: opendir(), readdir()
- ✓ Non-blocking I/O with EAGAIN handling

### Synchronization Primitives
- ✓ Mutex: Work queue protection
- ✓ Condition Variables: Producer-consumer coordination
- ✓ Barrier: Thread startup synchronization
- ✓ Semaphore: Rate limiting
- ✓ RWLock: Statistics protection

### Signal Handling
- ✓ SIGINT/SIGTERM for graceful shutdown
- ✓ Resource cleanup on shutdown
- ✓ Thread coordination during shutdown

### Concurrency Patterns
- ✓ Thread pool with work queue
- ✓ Producer-consumer pattern
- ✓ Load balancing across workers
- ✓ Concurrent request/file handling

## File Structure

```
apps/
├── README.md                 # Detailed documentation
├── QUICKSTART.md            # This file
├── http_server.c            # Web server implementation
├── http_client.c            # Load testing client
├── file_processor.c         # File analysis tool
├── run_http_server.sh       # Server launcher script
├── run_http_client.sh       # Client launcher script
├── demo.sh                  # Automated demo
└── test_all_apps.sh         # Comprehensive test suite

www/                         # Web server content
├── index.html
├── test.html
├── style.css
├── data.json
└── info.txt

input/                       # Sample files for processor
├── sample1.txt
├── sample2.txt
└── sample3.txt

output/                      # Generated analysis reports
└── (created by file_processor)
```

## Expected Output

### HTTP Server
```
===========================================
LIBAPTH HTTP Server
===========================================
Port:         8080
Workers:      4
WWW Root:     ./www
===========================================

[Acceptor] Started on port 8080
[Worker 0] Started
[Worker 1] Started
[Worker 2] Started
[Worker 3] Started
[Stats] Started
[Server] Ready to accept connections
```

### HTTP Client
```
===========================================
LIBAPTH HTTP Client
===========================================
Host:                localhost
Port:                8080
Threads:             10
Requests per thread: 100
Total requests:      1000
===========================================

[Client] Starting load test...
...
===========================================
Test Results
===========================================
Total requests:      1000
Successful:          1000
Failed:              0
Bytes received:      245678
Elapsed time:        2.34 seconds
Requests per second: 427.35
===========================================
```

### File Processor
```
===========================================
LIBAPTH File Processor
===========================================
Input Dir:  ./input
Output Dir: ./output
Workers:    4
===========================================

[Scanner] Scanning input directory...
[Scanner] Found 3 files to process

[Worker 0] Started
[Worker 1] Started
[Worker 2] Started
[Worker 3] Started
[Worker 0] Processing: ./input/sample1.txt
...
===========================================
Processing Complete
===========================================
Total files processed: 3
Total bytes read:      5432
Total words counted:   789
===========================================
```

## Troubleshooting

**"cannot open shared object file"**
- Ensure LD_LIBRARY_PATH includes build/lib
- Run from project root or use absolute paths

**"Address already in use"**
- Port 8080 is in use, try a different port
- Kill existing server: `pkill -f http_server`

**"No such file or directory"**
- Ensure you've built the applications: `make apps`
- Check that www/ and input/ directories exist

**Segmentation fault**
- Verify LD_PRELOAD is set correctly
- Rebuild: `make clean && make all && make apps`

## Performance Tips

1. **Increase workers** for CPU-bound workloads
2. **Increase threads** in client for higher load
3. **Monitor with** `htop` or `perf stat` to see context switches
4. **Compare** with pthread versions to see LIBAPTH benefits

## Next Steps

- Modify applications to test specific features
- Add new applications for different workloads
- Benchmark against pthread implementations
- Profile with perf to analyze performance

For detailed documentation, see README.md in this directory.
