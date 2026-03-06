# LIBAPTH Applications

This directory contains real-world applications built to test and demonstrate the capabilities of LIBAPTH.

## Applications

### 1. HTTP Server (`http_server.c`)

A multi-threaded HTTP web server that demonstrates:
- Socket I/O operations (accept, recv, send)
- File I/O operations (serving static files)
- Thread pool architecture with work queue
- Synchronization primitives (mutex, condition variables, rwlock)
- Signal handling for graceful shutdown
- Real-time statistics tracking

**Usage:**
```bash
# Build the applications
make apps

# Run the server (with LD_PRELOAD to enable LIBAPTH hooks)
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/http_server [port] [num_workers] [www_root]

# Example: Run on port 8080 with 4 workers, serving files from ./www
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/http_server 8080 4 ./www
```

**Default values:**
- Port: 8080
- Workers: 4
- WWW Root: ./www

**Testing:**
Open your browser and navigate to `http://localhost:8080/`

The server includes sample HTML, CSS, JSON, and text files in the `www/` directory.

### 2. HTTP Client (`http_client.c`)

A multi-threaded HTTP client for load testing that demonstrates:
- Concurrent socket connections
- Thread synchronization with barriers
- Statistics collection with atomic operations
- Performance measurement

**Usage:**
```bash
# Run the client (with LD_PRELOAD to enable LIBAPTH hooks)
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/http_client <host> <port> <num_threads> <requests_per_thread>

# Example: 10 threads, 100 requests each = 1000 total requests
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/http_client localhost 8080 10 100
```

**Default values:**
- Host: localhost
- Port: 8080
- Threads: 10
- Requests per thread: 100

### 3. File Processor (`file_processor.c`)

A concurrent file processing application that demonstrates:
- File I/O operations (read, write)
- Thread pool with work queue
- Semaphore for rate limiting (max concurrent files)
- Read-write locks for shared data structures
- Thread-specific data (worker IDs)
- Directory scanning and file analysis

**Usage:**
```bash
# Run the file processor (with LD_PRELOAD to enable LIBAPTH hooks)
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/file_processor <input_dir> <output_dir> <num_workers>

# Example: Process files from ./input with 4 workers
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/file_processor ./input ./output 4
```

**Default values:**
- Input Dir: ./input
- Output Dir: ./output
- Workers: 4

**What it does:**
- Scans the input directory for text files
- Processes each file concurrently using a thread pool
- Analyzes word count, character frequency, line count
- Generates detailed reports in the output directory
- Limits concurrent file processing to avoid resource exhaustion

**Testing:**
Sample input files are provided in the `input/` directory. Run the processor
and check the `output/` directory for analysis reports.

### 4. Producer-Consumer (`producer_consumer.c`)

Classic producer-consumer problem demonstrating:
- Multiple producer and consumer threads
- Bounded buffer with mutex and condition variables
- Thread-specific data
- Once-only initialization (apth_once)
- Thread cancellation testing
- Atomic operations for statistics

**Usage:**
```bash
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/producer_consumer <num_producers> <num_consumers> <buffer_size> <items_per_producer>

# Example: 3 producers, 2 consumers, buffer size 10, 20 items each
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/producer_consumer 3 2 10 20
```

**Default values:**
- Producers: 3
- Consumers: 2
- Buffer size: 10
- Items per producer: 20

### 5. Readers-Writers (`readers_writers.c`)

Readers-writers problem demonstrating:
- Read-write locks (apth_rwlock)
- Multiple readers accessing data concurrently
- Exclusive writer access
- Barrier synchronization for coordinated start
- Correctness verification (detects violations)

**Usage:**
```bash
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/readers_writers <num_readers> <num_writers> <iterations>

# Example: 5 readers, 2 writers, 10 iterations each
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/readers_writers 5 2 10
```

**Default values:**
- Readers: 5
- Writers: 2
- Iterations: 10

### 6. Dining Philosophers (`dining_philosophers.c`)

Classic dining philosophers problem demonstrating:
- Deadlock avoidance strategies
- Mutex for resource (fork) synchronization
- Semaphore for limiting concurrent diners
- Resource ordering to prevent deadlock
- Thread lifecycle management

**Usage:**
```bash
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/dining_philosophers <num_philosophers> <meals_per_philosopher>

# Example: 5 philosophers, 10 meals each
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/dining_philosophers 5 10
```

**Default values:**
- Philosophers: 5
- Meals per philosopher: 10

### 7. Signal Demo (`signal_demo.c`)

Signal handling and thread cancellation demonstration:
- Signal handling (SIGUSR1, SIGUSR2, SIGINT, SIGTERM)
- Thread cancellation (apth_cancel)
- Cleanup handlers (apth_cleanup_push/pop)
- Signal masks per thread (apth_sigmask)
- Thread-directed signals (apth_kill)
- Signal waiting (sigwait)

**Usage:**
```bash
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/signal_demo <num_workers> <duration>

# Example: 4 workers, run for 30 seconds
LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
LD_PRELOAD=build/lib/libapth.so \
build/bin/signal_demo 4 30
```

**Default values:**
- Workers: 4
- Duration: 30 seconds

## Complete Testing Workflow

1. **Build everything:**
   ```bash
   make clean
   make all
   make apps
   ```

2. **Start the server in one terminal:**
   ```bash
   LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
   LD_PRELOAD=build/lib/libapth.so \
   build/bin/http_server 8080 4 ./www
   ```

3. **Run the client in another terminal:**
   ```bash
   LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
   LD_PRELOAD=build/lib/libapth.so \
   build/bin/http_client localhost 8080 20 500
   ```

4. **Monitor the server output** to see:
   - Connection logs
   - Request handling
   - Statistics updates every 10 seconds
   - Worker thread activity

5. **Stop the server gracefully** with Ctrl+C (SIGINT)

6. **Test other applications:**
   ```bash
   # Producer-Consumer
   LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
   LD_PRELOAD=build/lib/libapth.so \
   build/bin/producer_consumer 3 2 10 20

   # Readers-Writers
   LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
   LD_PRELOAD=build/lib/libapth.so \
   build/bin/readers_writers 5 2 10

   # Dining Philosophers
   LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
   LD_PRELOAD=build/lib/libapth.so \
   build/bin/dining_philosophers 5 10

   # Signal Demo
   LD_LIBRARY_PATH=build/lib:$LD_LIBRARY_PATH \
   LD_PRELOAD=build/lib/libapth.so \
   build/bin/signal_demo 4 30
   ```

## What These Applications Test

### I/O Operations
- **HTTP Server/Client**: Socket I/O (accept, connect, send, recv)
- **File Processor**: File I/O (open, read, write, close), directory operations

### Thread Management
- **All applications**: Thread creation (apth_create), joining (apth_join)
- **HTTP Server, File Processor**: Thread pools with worker threads
- **Signal Demo**: Thread cancellation (apth_cancel), cleanup handlers

### Synchronization Primitives
- **Mutex**: All applications use mutexes for protecting shared state
- **Condition Variables**: HTTP Server, File Processor, Producer-Consumer
- **Barrier**: HTTP Client, Readers-Writers (coordinated start)
- **Semaphore**: File Processor (rate limiting), Dining Philosophers (table limit)
- **RWLock**: HTTP Server (stats), Readers-Writers (shared data)

### Advanced Features
- **Thread-specific data**: File Processor, Producer-Consumer
- **Once-only initialization**: Producer-Consumer (apth_once)
- **Signal handling**: HTTP Server, Signal Demo
- **Signal masks**: Signal Demo (apth_sigmask)
- **Thread-directed signals**: Signal Demo (apth_kill)
- **Atomic operations**: All applications for statistics

### Classic Concurrency Problems
- **Producer-Consumer**: Bounded buffer problem
- **Readers-Writers**: Concurrent read/exclusive write problem
- **Dining Philosophers**: Resource allocation and deadlock avoidance
- Acceptor thread pattern for handling connections

### I/O Operations
- **Socket I/O**: accept(), connect(), send(), recv()
- **File I/O**: open(), read(), close()
- All operations are hooked by LIBAPTH for optimization

### Synchronization Primitives
- **Mutex**: Protecting work queue access
- **Condition Variables**: Producer-consumer coordination
- **Barrier**: Synchronizing client thread startup
- **RWLock**: Statistics protection (demonstrated in server)

### Signal Handling
- SIGINT and SIGTERM for graceful shutdown
- Proper cleanup of resources
- Thread coordination during shutdown

### Concurrency Patterns
- Thread pool with work queue
- Producer-consumer pattern
- Load balancing across workers
- Concurrent request handling

## Performance Comparison

You can compare LIBAPTH performance against standard pthreads by running the applications without LD_PRELOAD (though this won't work correctly as the applications are linked against libapth).

For a fair comparison, you would need to:
1. Create pthread versions of these applications
2. Run both versions with the same workload
3. Measure:
   - Requests per second
   - Context switches (using `perf stat`)
   - CPU utilization
   - Latency distribution

## Troubleshooting

**Server won't start:**
- Check if port is already in use: `netstat -tuln | grep 8080`
- Try a different port: `./http_server 9090`

**Connection refused:**
- Ensure server is running
- Check firewall settings
- Verify correct host/port in client

**Segmentation fault:**
- Ensure LD_PRELOAD is set correctly
- Check that libapth.so is in the LD_LIBRARY_PATH
- Verify the library was built successfully

**Poor performance:**
- Increase number of workers
- Check system resource limits (ulimit -n)
- Monitor CPU and memory usage

## Future Enhancements

Potential additions to these applications:
- HTTP POST/PUT support
- Keep-alive connections
- Chunked transfer encoding
- HTTPS support
- Request routing
- Caching layer
- Database integration
- WebSocket support
