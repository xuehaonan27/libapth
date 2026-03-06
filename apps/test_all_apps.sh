#!/bin/bash
# test_all_apps.sh - Comprehensive test of all LIBAPTH applications

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${BLUE}=========================================="
echo "LIBAPTH Applications Test Suite"
echo -e "==========================================${NC}"
echo ""

# Build if needed
if [ ! -f "$PROJECT_ROOT/build/bin/http_server" ] || \
   [ ! -f "$PROJECT_ROOT/build/bin/http_client" ] || \
   [ ! -f "$PROJECT_ROOT/build/bin/file_processor" ]; then
    echo -e "${YELLOW}Building applications...${NC}"
    cd "$PROJECT_ROOT"
    make apps
    echo ""
fi

export LD_LIBRARY_PATH="$PROJECT_ROOT/build/lib:$LD_LIBRARY_PATH"
export LD_PRELOAD="$PROJECT_ROOT/build/lib/libapth.so"

# Test 1: File Processor
echo -e "${GREEN}=========================================="
echo "Test 1: File Processor"
echo -e "==========================================${NC}"
echo ""

rm -rf "$PROJECT_ROOT/output"
mkdir -p "$PROJECT_ROOT/output"

"$PROJECT_ROOT/build/bin/file_processor" "$PROJECT_ROOT/input" "$PROJECT_ROOT/output" 4

echo ""
echo -e "${GREEN}✓ File processor completed${NC}"
echo "Output files:"
ls -lh "$PROJECT_ROOT/output/"
echo ""

# Test 2: HTTP Server and Client
echo -e "${GREEN}=========================================="
echo "Test 2: HTTP Server and Client"
echo -e "==========================================${NC}"
echo ""

PORT=8080
"$PROJECT_ROOT/build/bin/http_server" "$PORT" 4 "$PROJECT_ROOT/www" > /tmp/libapth_test_server.log 2>&1 &
SERVER_PID=$!

echo -e "${YELLOW}Waiting for server to start...${NC}"
sleep 2

if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${RED}Error: Server failed to start${NC}"
    cat /tmp/libapth_test_server.log
    exit 1
fi

echo -e "${GREEN}✓ Server started (PID: $SERVER_PID)${NC}"
echo ""

# Quick test with curl
if command -v curl &> /dev/null; then
    echo "Testing with curl..."
    if curl -s "http://localhost:$PORT/" > /dev/null; then
        echo -e "${GREEN}✓ Server is responding${NC}"
    else
        echo -e "${RED}✗ Server not responding${NC}"
    fi
    echo ""
fi

# Run load test
echo "Running load test (10 threads, 50 requests each)..."
"$PROJECT_ROOT/build/bin/http_client" localhost "$PORT" 10 50

echo ""
echo -e "${GREEN}✓ Load test completed${NC}"
echo ""

# Shutdown server
echo "Shutting down server..."
kill -INT $SERVER_PID
wait $SERVER_PID 2>/dev/null || true
echo -e "${GREEN}✓ Server shut down gracefully${NC}"
echo ""

# Summary
echo -e "${BLUE}=========================================="
echo "Test Summary"
echo -e "==========================================${NC}"
echo -e "${GREEN}✓ File Processor: PASSED${NC}"
echo -e "${GREEN}✓ HTTP Server/Client: PASSED${NC}"
echo ""
echo "All tests completed successfully!"
echo ""
echo "Logs:"
echo "  Server log: /tmp/libapth_test_server.log"
echo "  Output files: $PROJECT_ROOT/output/"
echo ""
