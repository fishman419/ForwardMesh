#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ASAN_BIN_DIR="${ASAN_BIN:-$PROJECT_DIR/bin_asan}"

WORK_ROOT="$(mktemp -d)"
BASE_PORT=$(( 46200 + ($$ % 2000) ))
PIDS=()

cleanup() {
    local status=$?
    if [ "$status" -ne 0 ]; then
        for logf in "$WORK_ROOT"/*/forward_server.log; do
            [ -f "$logf" ] && echo "--- $logf ---" && cat "$logf"
        done
    fi
    for pid in "${PIDS[@]:-}"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$WORK_ROOT"
}
trap cleanup EXIT

start_asan_node() {
    local dir="$1"
    local port="$2"
    local pidfile="$dir/fwdd.pid"
    mkdir -p "$dir"
    "$ASAN_BIN_DIR/fwdd" -F -d "$dir" -p "$port" -t 4 -P "$pidfile" &
    NODE_STARTED_PID=$!
    PIDS+=("$NODE_STARTED_PID")
    for _ in $(seq 1 50); do
        [ -s "$pidfile" ] && return 0
        sleep 0.05
    done
    return 1
}

echo "=== ForwardMesh ASAN Memory Tests (Protocol V2) ==="

# Test 1: Single-node push & pull
echo "[Test 1] Single-node push & pull with ASAN..."
N1="$WORK_ROOT/n1"
SRC="$WORK_ROOT/src.txt"
PULLED="$WORK_ROOT/pulled.txt"
mkdir -p "$N1"
printf "asan test payload\n" > "$SRC"

start_asan_node "$N1" $((BASE_PORT + 1))
P1="$NODE_STARTED_PID"

"$ASAN_BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 1)) -f "$SRC"
cmp -s "$SRC" "$N1/src.txt"

"$ASAN_BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 1)) -g src.txt -f "$PULLED"
cmp -s "$SRC" "$PULLED"

kill "$P1" && wait "$P1"
echo "  PASS: Single-node push & pull clean under ASAN"

# Test 2: Two-node streaming, tee caching (-s), and error path
echo "[Test 2] Two-node streaming, tee caching & error cleanup with ASAN..."
C1="$WORK_ROOT/c1"
C2="$WORK_ROOT/c2"
mkdir -p "$C1" "$C2"
dd if=/dev/urandom of="$WORK_ROOT/blob.bin" bs=1024 count=128 2>/dev/null

start_asan_node "$C2" $((BASE_PORT + 12)); PC2="$NODE_STARTED_PID"
start_asan_node "$C1" $((BASE_PORT + 11)); PC1="$NODE_STARTED_PID"

# Cut-through streaming (no cache on C1)
"$ASAN_BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 11)),127.0.0.1:$((BASE_PORT + 12)) -f "$WORK_ROOT/blob.bin"
cmp -s "$WORK_ROOT/blob.bin" "$C2/blob.bin"
[ ! -e "$C1/blob.bin" ]

# Tee caching (-s)
"$ASAN_BIN_DIR/fwd" -s -a 127.0.0.1:$((BASE_PORT + 11)),127.0.0.1:$((BASE_PORT + 12)) -g blob.bin -f "$WORK_ROOT/blob_out.bin"
cmp -s "$WORK_ROOT/blob.bin" "$WORK_ROOT/blob_out.bin"
cmp -s "$WORK_ROOT/blob.bin" "$C1/blob.bin"

if "$ASAN_BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 11)),127.0.0.1:$((BASE_PORT + 12)) -g nonexistent.txt -f "$WORK_ROOT/err.txt" > /dev/null 2>&1; then
    echo "  FAIL: Expected non-zero exit on missing file"
    exit 1
fi

kill "$PC1" "$PC2"
wait "$PC1" "$PC2"
echo "  PASS: Two-node streaming, tee caching & error cleanup clean under ASAN"

echo ""
echo "=== All ASAN tests passed (no memory errors detected) ==="
