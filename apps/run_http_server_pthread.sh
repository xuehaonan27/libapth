#!/bin/bash
# run_http_server.sh - Helper script to run the HTTP server with LIBAPTH

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Default values
PORT=${1:-8080}
WORKERS=${2:-4}
WWW_ROOT=${3:-"$PROJECT_ROOT/www"}

echo "=========================================="
echo "Starting LIBPTHREAD HTTP Server"
echo "=========================================="
echo "Port:        $PORT"
echo "Workers:     $WORKERS"
echo "WWW Root:    $WWW_ROOT"
echo "=========================================="
echo ""

# Run the server
exec "$PROJECT_ROOT/build/bin/http_server_pthread" "$PORT" "$WORKERS" "$WWW_ROOT"
