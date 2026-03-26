#!/bin/bash
#
# run_tests.sh — Unified test runner for LIBAPTH
#
# Reads test/test_manifest.txt and runs each test binary with output
# capture, timeouts, expected-failure handling, and clean one-line
# per-test reporting.
#
# Usage:
#   ./test/run_tests.sh                  # run all tests
#   ./test/run_tests.sh test_mutex       # run one test by name
#   ./test/run_tests.sh --filter io      # run tests matching pattern
#
# Exit code: 0 if all pass/xfail/skip, 1 if any fail/timeout.

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_DIR="$PROJECT_ROOT/build/bin"
LIB_DIR="$PROJECT_ROOT/build/lib"
MANIFEST="$SCRIPT_DIR/test_manifest.txt"
SHARED_LIB="$LIB_DIR/libapth.so"

# Colors (disabled if not a terminal)
if [ -t 1 ]; then
    GREEN='\033[0;32m'
    RED='\033[0;31m'
    YELLOW='\033[0;33m'
    CYAN='\033[0;36m'
    GRAY='\033[0;90m'
    BOLD='\033[1m'
    RESET='\033[0m'
else
    GREEN='' RED='' YELLOW='' CYAN='' GRAY='' BOLD='' RESET=''
fi

# Counters
PASS_COUNT=0
FAIL_COUNT=0
XFAIL_COUNT=0
SKIP_COUNT=0
TIMEOUT_COUNT=0
TOTAL_COUNT=0

# Filter pattern (optional CLI arg)
FILTER="${1:-}"

# ================================================================
# Run a single test
# ================================================================
run_test() {
    local binary="$1"
    local env_mode="$2"
    local timeout_sec="$3"
    shift 3
    local flags="$*"

    local binary_path="$BIN_DIR/$binary"
    local test_name="$binary"
    TOTAL_COUNT=$((TOTAL_COUNT + 1))

    # Check if binary exists
    if [ ! -f "$binary_path" ]; then
        # Check skip conditions
        if echo "$flags" | grep -q "skip_unless=rdma"; then
            printf "  ${CYAN}[SKIP]${RESET}    %-40s (requires rdma build)\n" "$test_name"
            SKIP_COUNT=$((SKIP_COUNT + 1))
            return 0
        fi
        printf "  ${YELLOW}[SKIP]${RESET}    %-40s (binary not found)\n" "$test_name"
        SKIP_COUNT=$((SKIP_COUNT + 1))
        return 0
    fi

    # Check skip_unless conditions
    if echo "$flags" | grep -q "skip_unless=rdma"; then
        if [ ! -f "$binary_path" ]; then
            printf "  ${CYAN}[SKIP]${RESET}    %-40s (requires rdma)\n" "$test_name"
            SKIP_COUNT=$((SKIP_COUNT + 1))
            return 0
        fi
    fi

    # Build environment
    local env_vars=""
    if [ "$env_mode" = "preload" ]; then
        env_vars="LD_LIBRARY_PATH=$LIB_DIR:\$LD_LIBRARY_PATH LD_PRELOAD=$SHARED_LIB"
    fi

    # Parse xfail flag
    local xfail_code=""
    if echo "$flags" | grep -qoE 'xfail=[0-9]+'; then
        xfail_code=$(echo "$flags" | grep -oE 'xfail=[0-9]+' | cut -d= -f2)
    fi

    # Parse long_output flag
    local long_output=0
    if echo "$flags" | grep -q "long_output"; then
        long_output=1
    fi

    # Run with timeout, capture output
    local tmpfile
    tmpfile=$(mktemp /tmp/apth_test_XXXXXX)

    local start_time
    start_time=$(date +%s%N 2>/dev/null || date +%s)

    # Run the test, suppressing shell signal messages for expected crashes
    if [ "$env_mode" = "preload" ]; then
        (timeout --signal=KILL "$timeout_sec" env \
            LD_LIBRARY_PATH="$LIB_DIR:${LD_LIBRARY_PATH:-}" \
            LD_PRELOAD="$SHARED_LIB" \
            "$binary_path" >"$tmpfile" 2>&1)  2>/dev/null
    else
        (timeout --signal=KILL "$timeout_sec" "$binary_path" >"$tmpfile" 2>&1)  2>/dev/null
    fi
    local rc=$?

    local end_time
    end_time=$(date +%s%N 2>/dev/null || date +%s)

    # Calculate elapsed time
    local elapsed_ms
    if [ ${#start_time} -gt 10 ]; then
        elapsed_ms=$(( (end_time - start_time) / 1000000 ))
    else
        elapsed_ms=$(( (end_time - start_time) * 1000 ))
    fi
    local elapsed_s
    elapsed_s=$(awk "BEGIN {printf \"%.1f\", $elapsed_ms / 1000.0}")

    # Determine verdict
    local verdict=""
    local detail=""

    if [ $rc -eq 137 ]; then
        # Killed by SIGKILL (from timeout)
        verdict="TIMEOUT"
        detail="killed after ${timeout_sec}s"
    elif [ -n "$xfail_code" ]; then
        if [ $rc -eq "$xfail_code" ]; then
            verdict="XFAIL"
            detail="expected exit $xfail_code"
        elif [ $rc -eq 0 ]; then
            verdict="XPASS"
            detail="unexpectedly passed (expected exit $xfail_code)"
        else
            verdict="FAIL"
            detail="exit=$rc (expected $xfail_code)"
        fi
    elif [ $rc -eq 0 ]; then
        verdict="PASS"
    else
        verdict="FAIL"
        detail="exit=$rc"
    fi

    # Print verdict
    case "$verdict" in
        PASS)
            printf "  ${GREEN}[PASS]${RESET}    %-40s ${GRAY}(${elapsed_s}s)${RESET}\n" "$test_name"
            PASS_COUNT=$((PASS_COUNT + 1))
            ;;
        FAIL)
            printf "  ${RED}[FAIL]${RESET}    %-40s ${GRAY}(${elapsed_s}s, ${detail})${RESET}\n" "$test_name"
            FAIL_COUNT=$((FAIL_COUNT + 1))
            # Show truncated output for failures
            local lines
            lines=$(wc -l < "$tmpfile")
            if [ "$long_output" -eq 1 ] && [ "$lines" -gt 40 ]; then
                echo "          --- output (first 10 + last 5 of $lines lines) ---"
                head -10 "$tmpfile" | sed 's/^/          | /'
                echo "          | ... ($((lines - 15)) lines omitted) ..."
                tail -5 "$tmpfile" | sed 's/^/          | /'
            elif [ "$lines" -gt 0 ] && [ "$lines" -le 30 ]; then
                echo "          --- output ($lines lines) ---"
                sed 's/^/          | /' "$tmpfile"
            elif [ "$lines" -gt 30 ]; then
                echo "          --- output (first 15 + last 5 of $lines lines) ---"
                head -15 "$tmpfile" | sed 's/^/          | /'
                echo "          | ... ($((lines - 20)) lines omitted) ..."
                tail -5 "$tmpfile" | sed 's/^/          | /'
            fi
            ;;
        XFAIL)
            printf "  ${YELLOW}[XFAIL]${RESET}   %-40s ${GRAY}(${elapsed_s}s, ${detail})${RESET}\n" "$test_name"
            XFAIL_COUNT=$((XFAIL_COUNT + 1))
            ;;
        XPASS)
            printf "  ${YELLOW}[XPASS]${RESET}   %-40s ${GRAY}(${elapsed_s}s, ${detail})${RESET}\n" "$test_name"
            # XPASS is a warning, not a failure
            PASS_COUNT=$((PASS_COUNT + 1))
            ;;
        TIMEOUT)
            printf "  ${RED}[TIMEOUT]${RESET} %-40s ${GRAY}(${detail})${RESET}\n" "$test_name"
            TIMEOUT_COUNT=$((TIMEOUT_COUNT + 1))
            ;;
        SKIP)
            printf "  ${CYAN}[SKIP]${RESET}    %-40s ${GRAY}(${detail})${RESET}\n" "$test_name"
            SKIP_COUNT=$((SKIP_COUNT + 1))
            ;;
    esac

    rm -f "$tmpfile"
    return 0
}

# ================================================================
# Main
# ================================================================

if [ ! -f "$MANIFEST" ]; then
    echo "ERROR: Test manifest not found: $MANIFEST" >&2
    echo "Run 'make tests' first to build test binaries." >&2
    exit 1
fi

echo "=========================================="
echo "LIBAPTH Test Suite"
echo "=========================================="
echo ""

SUITE_START=$(date +%s)

while IFS= read -r line; do
    # Skip comments and empty lines
    [[ "$line" =~ ^[[:space:]]*# ]] && continue
    [[ -z "${line// }" ]] && continue

    # Parse: BINARY ENV TIMEOUT [FLAGS...]
    local_binary=$(echo "$line" | awk '{print $1}')
    local_env=$(echo "$line" | awk '{print $2}')
    local_timeout=$(echo "$line" | awk '{print $3}')
    local_flags=$(echo "$line" | awk '{for(i=4;i<=NF;i++) printf "%s ", $i}')

    # Apply filter if provided
    if [ -n "$FILTER" ] && ! echo "$local_binary" | grep -qi "$FILTER"; then
        continue
    fi

    run_test "$local_binary" "$local_env" "$local_timeout" $local_flags

done < "$MANIFEST"

SUITE_END=$(date +%s)
SUITE_ELAPSED=$((SUITE_END - SUITE_START))

echo ""
echo "=========================================="
printf "Results: ${GREEN}%d passed${RESET}" "$PASS_COUNT"
[ "$XFAIL_COUNT" -gt 0 ] && printf ", ${YELLOW}%d xfail${RESET}" "$XFAIL_COUNT"
[ "$FAIL_COUNT" -gt 0 ] && printf ", ${RED}%d failed${RESET}" "$FAIL_COUNT"
[ "$TIMEOUT_COUNT" -gt 0 ] && printf ", ${RED}%d timeout${RESET}" "$TIMEOUT_COUNT"
[ "$SKIP_COUNT" -gt 0 ] && printf ", ${CYAN}%d skipped${RESET}" "$SKIP_COUNT"
printf " (${TOTAL_COUNT} total)\n"
echo "Total time: ${SUITE_ELAPSED}s"
echo "=========================================="

# Exit code: fail if any FAIL or TIMEOUT
if [ "$FAIL_COUNT" -gt 0 ] || [ "$TIMEOUT_COUNT" -gt 0 ]; then
    exit 1
fi
exit 0
