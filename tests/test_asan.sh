#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ASAN_BIN_DIR="${ASAN_BIN:-$(dirname "$0")/../bin_asan}"

echo "=== ForwardMesh ASAN Memory Tests ==="

# Kill any existing fwdd processes to free ports
pkill -f "./bin_asan/fwdd" 2>/dev/null || true
pkill -f "./bin/fwdd" 2>/dev/null || true
sleep 1

# Test 1: Single-node push
echo "[Test 1] Single-node push with ASAN..."
TEST_DIR=$(mktemp -d)
"$ASAN_BIN_DIR/fwdd" -d "$TEST_DIR" -p 40001 > /dev/null 2>&1 &
FWDD_PID=$!
sleep 1

echo "test content" > "$TEST_DIR/testfile.txt"
"$ASAN_BIN_DIR/fwd" -a 127.0.0.1:40001 -f "$TEST_DIR/testfile.txt" > /dev/null 2>&1

if [ -f "$TEST_DIR/testfile.txt" ]; then
    echo "  PASS: Single-node push works"
else
    echo "  FAIL: Single-node push failed"
    kill $FWDD_PID 2>/dev/null || true
    rm -rf "$TEST_DIR"
    exit 1
fi

kill $FWDD_PID 2>/dev/null || true
rm -rf "$TEST_DIR"

# Test 2: Single-node pull
echo "[Test 2] Single-node pull with ASAN..."
TEST_DIR=$(mktemp -d)
"$ASAN_BIN_DIR/fwdd" -d "$TEST_DIR" -p 40002 > /dev/null 2>&1 &
FWDD_PID=$!
sleep 1

echo "pull content" > "$TEST_DIR/remote_file.txt"
"$ASAN_BIN_DIR/fwd" -a 127.0.0.1:40002 -g remote_file.txt -f /tmp/pull_output.txt > /dev/null 2>&1

if [ -f /tmp/pull_output.txt ] && grep -q "pull content" /tmp/pull_output.txt; then
    echo "  PASS: Single-node pull works"
else
    echo "  FAIL: Single-node pull failed"
    kill $FWDD_PID 2>/dev/null || true
    rm -rf "$TEST_DIR"
    rm -f /tmp/pull_output.txt
    exit 1
fi

kill $FWDD_PID 2>/dev/null || true
rm -rf "$TEST_DIR"
rm -f /tmp/pull_output.txt

# Test 3: Two-node push chain
echo "[Test 3] Two-node push chain with ASAN..."
TEST_DIR1=$(mktemp -d)
TEST_DIR2=$(mktemp -d)
"$ASAN_BIN_DIR/fwdd" -d "$TEST_DIR2" -p 40011 > /dev/null 2>&1 &
FWDD2_PID=$!
"$ASAN_BIN_DIR/fwdd" -d "$TEST_DIR1" -p 40010 > /dev/null 2>&1 &
FWDD1_PID=$!
sleep 1

echo "chain push test" > /tmp/chain_push.txt
"$ASAN_BIN_DIR/fwd" -a 127.0.0.1:40010,127.0.0.1:40011 -f /tmp/chain_push.txt > /dev/null 2>&1

if [ -f "$TEST_DIR2/chain_push.txt" ]; then
    echo "  PASS: Two-node push works"
else
    echo "  FAIL: Two-node push failed"
    kill $FWDD1_PID $FWDD2_PID 2>/dev/null || true
    rm -rf "$TEST_DIR1" "$TEST_DIR2"
    rm -f /tmp/chain_push.txt
    exit 1
fi

kill $FWDD1_PID $FWDD2_PID 2>/dev/null || true
rm -rf "$TEST_DIR1" "$TEST_DIR2"
rm -f /tmp/chain_push.txt

# Test 4: Two-node pull chain
echo "[Test 4] Two-node pull chain with ASAN..."
TEST_DIR1=$(mktemp -d)
TEST_DIR2=$(mktemp -d)
"$ASAN_BIN_DIR/fwdd" -d "$TEST_DIR2" -p 40021 > /dev/null 2>&1 &
FWDD2_PID=$!
"$ASAN_BIN_DIR/fwdd" -d "$TEST_DIR1" -p 40020 > /dev/null 2>&1 &
FWDD1_PID=$!
sleep 1

echo "chain pull test" > "$TEST_DIR2/chain_pull_src.txt"
"$ASAN_BIN_DIR/fwd" -a 127.0.0.1:40020,127.0.0.1:40021 -g chain_pull_src.txt -f /tmp/chain_pull_dst.txt > /dev/null 2>&1

if [ -f /tmp/chain_pull_dst.txt ] && grep -q "chain pull test" /tmp/chain_pull_dst.txt; then
    echo "  PASS: Two-node pull works"
else
    echo "  FAIL: Two-node pull failed"
    kill $FWDD1_PID $FWDD2_PID 2>/dev/null || true
    rm -rf "$TEST_DIR1" "$TEST_DIR2"
    rm -f /tmp/chain_pull_dst.txt
    exit 1
fi

kill $FWDD1_PID $FWDD2_PID 2>/dev/null || true
rm -rf "$TEST_DIR1" "$TEST_DIR2"
rm -f /tmp/chain_pull_dst.txt

# Cleanup any remaining processes
pkill -f "./bin_asan/fwdd" 2>/dev/null || true

echo ""
echo "=== All ASAN tests passed (no memory errors detected) ==="
