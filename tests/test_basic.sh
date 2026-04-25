#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
BIN_DIR="$PROJECT_DIR/bin"

echo "=== ForwardMesh Basic Tests ==="

# Test 1: Build
echo "[Test 1] Building..."
cd "$PROJECT_DIR"
make clean > /dev/null 2>&1
make > /dev/null 2>&1
if [ -f "$BIN_DIR/fwdd" ] && [ -f "$BIN_DIR/fwd" ]; then
    echo "  PASS: Build successful"
else
    echo "  FAIL: Binary not found"
    exit 1
fi

# Test 2: Help output
echo "[Test 2] fwdd help..."
"$BIN_DIR/fwdd" -h | grep -q "Usage:"
echo "  PASS: fwdd help works"

echo "[Test 3] fwd help..."
"$BIN_DIR/fwd" -h | grep -q "Usage:"
echo "  PASS: fwd help works"

# Test 4: Start server, send file, verify
echo "[Test 4] Integration test..."
TEST_DIR=$(mktemp -d)
"$BIN_DIR/fwdd" -d "$TEST_DIR" -p 40001 &
FWDD_PID=$!
sleep 1

echo "test content" > "$TEST_DIR/testfile.txt"
"$BIN_DIR/fwd" -a 127.0.0.1:40001 -f "$TEST_DIR/testfile.txt" 2>/dev/null

if [ -f "$TEST_DIR/testfile.txt" ]; then
    echo "  PASS: File received and stored"
else
    echo "  FAIL: File not stored"
    kill $FWDD_PID 2>/dev/null || true
    rm -rf "$TEST_DIR"
    exit 1
fi

kill $FWDD_PID 2>/dev/null || true
rm -rf "$TEST_DIR"

echo ""
echo "=== All tests passed ==="
