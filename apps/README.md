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

## What These Applications Test

### Thread Management
- Creating and joining multiple apth threads
- Thread pool pattern with worker threads
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
