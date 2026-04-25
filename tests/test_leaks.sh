#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LEAKS_BIN_DIR="${LEAKS_BIN:-$(dirname "$0")/../bin_leaks}"

echo "=== ForwardMesh Memory Leak Tests (macOS leaks) ==="

# Kill any existing fwdd processes to free ports
pkill -f "./bin_leaks/fwdd" 2>/dev/null || true
pkill -f "./bin/fwdd" 2>/dev/null || true
sleep 1

# Helper to run server and check leaks
run_with_leaks_check() {
    local test_name="$1"
    local test_func="$2"

    echo "[$test_name] Running with leak detection..."

    # Start server
    TEST_DIR=$(mktemp -d)
    SERVER_PID=$( "$LEAKS_BIN_DIR/fwdd" -d "$TEST_DIR" -p "$PORT" > /dev/null 2>&1 & echo $! )
    sleep 1

    # Run test
    eval "$test_func"
    local test_result=$?

    # Stop server and check for leaks
    kill $SERVER_PID 2>/dev/null || true
    sleep 1

    # Check for leaks using leaks utility
    LEAK_OUTPUT=$(leaks -q "$SERVER_PID" 2>&1 || true)
    rm -rf "$TEST_DIR"

    if echo "$LEAK_OUTPUT" | grep -q "leaks for"; then
        echo "  FAIL: Memory leaks detected:"
        echo "$LEAK_OUTPUT"
        return 1
    fi

    if [ $test_result -ne 0 ]; then
        echo "  FAIL: Test failed"
        return 1
    fi

    echo "  PASS: No memory leaks detected"
    return 0
}

PORT=40051

# Test 1: Single-node push
echo "[Test 1] Single-node push..."
TEST_DIR=$(mktemp -d)
"$LEAKS_BIN_DIR/fwdd" -d "$TEST_DIR" -p $PORT > /dev/null 2>&1 &
SERVER_PID=$!
sleep 1

echo "test content" > "$TEST_DIR/testfile.txt"
if "$LEAKS_BIN_DIR/fwd" -a 127.0.0.1:$PORT -f "$TEST_DIR/testfile.txt" > /dev/null 2>&1; then
    if [ -f "$TEST_DIR/testfile.txt" ]; then
        echo "  PASS: Single-node push works"
    else
        echo "  FAIL: File not stored"
    fi
else
    echo "  FAIL: Client failed"
fi

# Check leaks
sleep 1
LEAKS_OUTPUT=$(leaks -q $SERVER_PID 2>&1 || true)
kill $SERVER_PID 2>/dev/null || true
rm -rf "$TEST_DIR"

if echo "$LEAKS_OUTPUT" | grep -q "leaks for"; then
    echo "  LEAK: $LEAKS_OUTPUT"
fi

PORT=40052

# Test 2: Single-node pull
echo "[Test 2] Single-node pull..."
TEST_DIR=$(mktemp -d)
"$LEAKS_BIN_DIR/fwdd" -d "$TEST_DIR" -p $PORT > /dev/null 2>&1 &
SERVER_PID=$!
sleep 1

echo "pull content" > "$TEST_DIR/remote_file.txt"
if "$LEAKS_BIN_DIR/fwd" -a 127.0.0.1:$PORT -g remote_file.txt -f /tmp/pull_output.txt > /dev/null 2>&1; then
    if [ -f /tmp/pull_output.txt ] && grep -q "pull content" /tmp/pull_output.txt; then
        echo "  PASS: Single-node pull works"
    else
        echo "  FAIL: File not retrieved correctly"
    fi
else
    echo "  FAIL: Client failed"
fi

sleep 1
LEAKS_OUTPUT=$(leaks -q $SERVER_PID 2>&1 || true)
kill $SERVER_PID 2>/dev/null || true
rm -rf "$TEST_DIR"
rm -f /tmp/pull_output.txt

if echo "$LEAKS_OUTPUT" | grep -q "leaks for"; then
    echo "  LEAK: $LEAKS_OUTPUT"
fi

PORT=40053

# Test 3: Two-node push chain
echo "[Test 3] Two-node push chain..."
TEST_DIR1=$(mktemp -d)
TEST_DIR2=$(mktemp -d)
"$LEAKS_BIN_DIR/fwdd" -d "$TEST_DIR2" -p 40061 > /dev/null 2>&1 &
SERVER2_PID=$!
"$LEAKS_BIN_DIR/fwdd" -d "$TEST_DIR1" -p $PORT > /dev/null 2>&1 &
SERVER1_PID=$!
sleep 1

echo "chain push test" > /tmp/chain_push.txt
if "$LEAKS_BIN_DIR/fwd" -a 127.0.0.1:$PORT,127.0.0.1:40061 -f /tmp/chain_push.txt > /dev/null 2>&1; then
    if [ -f "$TEST_DIR2/chain_push.txt" ]; then
        echo "  PASS: Two-node push works"
    else
        echo "  FAIL: File not forwarded to second node"
    fi
else
    echo "  FAIL: Client failed"
fi

sleep 1
LEAKS_OUTPUT1=$(leaks -q $SERVER1_PID 2>&1 || true)
LEAKS_OUTPUT2=$(leaks -q $SERVER2_PID 2>&1 || true)
kill $SERVER1_PID $SERVER2_PID 2>/dev/null || true
rm -rf "$TEST_DIR1" "$TEST_DIR2"
rm -f /tmp/chain_push.txt

if echo "$LEAKS_OUTPUT1" | grep -q "leaks for"; then
    echo "  LEAK (server1): $LEAKS_OUTPUT1"
fi
if echo "$LEAKS_OUTPUT2" | grep -q "leaks for"; then
    echo "  LEAK (server2): $LEAKS_OUTPUT2"
fi

PORT=40054

# Test 4: Two-node pull chain
echo "[Test 4] Two-node pull chain..."
TEST_DIR1=$(mktemp -d)
TEST_DIR2=$(mktemp -d)
"$LEAKS_BIN_DIR/fwdd" -d "$TEST_DIR2" -p 40071 > /dev/null 2>&1 &
SERVER2_PID=$!
"$LEAKS_BIN_DIR/fwdd" -d "$TEST_DIR1" -p $PORT > /dev/null 2>&1 &
SERVER1_PID=$!
sleep 1

echo "chain pull test" > "$TEST_DIR2/chain_pull_src.txt"
if "$LEAKS_BIN_DIR/fwd" -a 127.0.0.1:$PORT,127.0.0.1:40071 -g chain_pull_src.txt -f /tmp/chain_pull_dst.txt > /dev/null 2>&1; then
    if [ -f /tmp/chain_pull_dst.txt ] && grep -q "chain pull test" /tmp/chain_pull_dst.txt; then
        echo "  PASS: Two-node pull works"
    else
        echo "  FAIL: File not retrieved correctly"
    fi
else
    echo "  FAIL: Client failed"
fi

sleep 1
LEAKS_OUTPUT1=$(leaks -q $SERVER1_PID 2>&1 || true)
LEAKS_OUTPUT2=$(leaks -q $SERVER2_PID 2>&1 || true)
kill $SERVER1_PID $SERVER2_PID 2>/dev/null || true
rm -rf "$TEST_DIR1" "$TEST_DIR2"
rm -f /tmp/chain_pull_dst.txt

if echo "$LEAKS_OUTPUT1" | grep -q "leaks for"; then
    echo "  LEAK (server1): $LEAKS_OUTPUT1"
fi
if echo "$LEAKS_OUTPUT2" | grep -q "leaks for"; then
    echo "  LEAK (server2): $LEAKS_OUTPUT2"
fi

# Cleanup
pkill -f "./bin_leaks/fwdd" 2>/dev/null || true

echo ""
echo "=== Leak tests completed ==="
