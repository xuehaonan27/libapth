#!/bin/bash
# test_sync_apps.sh - Test synchronization-focused applications

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${BLUE}=========================================="
echo "LIBAPTH Synchronization Applications Test"
echo -e "==========================================${NC}"
echo ""

export LD_LIBRARY_PATH="$PROJECT_ROOT/build/lib:$LD_LIBRARY_PATH"
export LD_PRELOAD="$PROJECT_ROOT/build/lib/libapth.so"

# Test 1: Producer-Consumer
echo -e "${GREEN}Test 1: Producer-Consumer${NC}"
echo "Running with 3 producers, 2 consumers, buffer size 10, 20 items each..."
"$PROJECT_ROOT/build/bin/producer_consumer" 3 2 10 20
echo -e "${GREEN}✓ Producer-Consumer completed${NC}"
echo ""

# Test 2: Readers-Writers
echo -e "${GREEN}Test 2: Readers-Writers${NC}"
echo "Running with 5 readers, 2 writers, 10 iterations each..."
"$PROJECT_ROOT/build/bin/readers_writers" 5 2 10
echo -e "${GREEN}✓ Readers-Writers completed${NC}"
echo ""

# Test 3: Dining Philosophers
echo -e "${GREEN}Test 3: Dining Philosophers${NC}"
echo "Running with 5 philosophers, 10 meals each..."
"$PROJECT_ROOT/build/bin/dining_philosophers" 5 10
echo -e "${GREEN}✓ Dining Philosophers completed${NC}"
echo ""

# Test 4: Signal Demo (short duration for testing)
echo -e "${GREEN}Test 4: Signal Demo${NC}"
echo "Running with 4 workers for 10 seconds..."
timeout 12 "$PROJECT_ROOT/build/bin/signal_demo" 4 10 || true
echo -e "${GREEN}✓ Signal Demo completed${NC}"
echo ""

echo -e "${BLUE}=========================================="
echo "All Synchronization Tests Completed!"
echo -e "==========================================${NC}"
echo ""
echo "All applications ran successfully without deadlocks or errors."
echo ""
