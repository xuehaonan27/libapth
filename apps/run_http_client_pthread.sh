#!/bin/bash
# run_http_client.sh - Helper script to run the HTTP client with LIBAPTH

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Default values
HOST=${1:-localhost}
PORT=${2:-8080}
THREADS=${3:-10}
REQUESTS=${4:-100}

echo "=========================================="
echo "Starting LIBPTHREAD HTTP Client"
echo "=========================================="
echo "Host:                $HOST"
echo "Port:                $PORT"
echo "Threads:             $THREADS"
echo "Requests per thread: $REQUESTS"
echo "Total requests:      $((THREADS * REQUESTS))"
echo "=========================================="
echo ""

# Run the client
exec "$PROJECT_ROOT/build/bin/http_client_pthread" "$HOST" "$PORT" "$THREADS" "$REQUESTS"
