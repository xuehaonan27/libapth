#!/bin/bash
# demo.sh - Automated demonstration of LIBAPTH HTTP server and client

set -e

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${BLUE}=========================================="
echo "LIBAPTH HTTP Server/Client Demo"
echo -e "==========================================${NC}"
echo ""

# Check if applications are built
if [ ! -f "$PROJECT_ROOT/build/bin/http_server" ] || [ ! -f "$PROJECT_ROOT/build/bin/http_client" ]; then
    echo -e "${YELLOW}Applications not found. Building...${NC}"
    cd "$PROJECT_ROOT"
    make apps
    echo ""
fi

# Set library paths
export LD_LIBRARY_PATH="$PROJECT_ROOT/build/lib:$LD_LIBRARY_PATH"
export LD_PRELOAD="$PROJECT_ROOT/build/lib/libapth.so"

# Configuration
PORT=8080
WORKERS=4
WWW_ROOT="$PROJECT_ROOT/www"
CLIENT_THREADS=10
CLIENT_REQUESTS=50

echo -e "${GREEN}Step 1: Starting HTTP Server${NC}"
echo "  Port: $PORT"
echo "  Workers: $WORKERS"
echo "  WWW Root: $WWW_ROOT"
echo ""

# Start server in background
"$PROJECT_ROOT/build/bin/http_server" "$PORT" "$WORKERS" "$WWW_ROOT" > /tmp/libapth_server.log 2>&1 &
SERVER_PID=$!

# Wait for server to start
echo -e "${YELLOW}Waiting for server to start...${NC}"
sleep 2

# Check if server is running
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${RED}Error: Server failed to start${NC}"
    cat /tmp/libapth_server.log
    exit 1
fi

echo -e "${GREEN}Server started successfully (PID: $SERVER_PID)${NC}"
echo ""

# Test with curl if available
if command -v curl &> /dev/null; then
    echo -e "${GREEN}Step 2: Testing with curl${NC}"
    echo -e "${YELLOW}Fetching http://localhost:$PORT/${NC}"
    curl -s "http://localhost:$PORT/" | head -20
    echo ""
    echo -e "${GREEN}✓ Server is responding${NC}"
    echo ""
fi

# Run client
echo -e "${GREEN}Step 3: Running Load Test${NC}"
echo "  Threads: $CLIENT_THREADS"
echo "  Requests per thread: $CLIENT_REQUESTS"
echo "  Total requests: $((CLIENT_THREADS * CLIENT_REQUESTS))"
echo ""

"$PROJECT_ROOT/build/bin/http_client" localhost "$PORT" "$CLIENT_THREADS" "$CLIENT_REQUESTS"

echo ""
echo -e "${GREEN}Step 4: Shutting down server${NC}"
kill -INT $SERVER_PID
wait $SERVER_PID 2>/dev/null || true

echo ""
echo -e "${BLUE}=========================================="
echo "Demo Complete!"
echo -e "==========================================${NC}"
echo ""
echo "Server log saved to: /tmp/libapth_server.log"
echo ""
echo "To run manually:"
echo "  Server: $SCRIPT_DIR/run_http_server.sh [port] [workers] [www_root]"
echo "  Client: $SCRIPT_DIR/run_http_client.sh [host] [port] [threads] [requests]"
echo ""
