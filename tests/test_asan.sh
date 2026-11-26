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
    local hb_ms="${3:-200}"
    local deny_peers="${4:-}"
    local pidfile="$dir/fwdd.pid"
    mkdir -p "$dir"
    FWDD_DENY_PEERS="$deny_peers" "$ASAN_BIN_DIR/fwdd" -F -d "$dir" -p "$port" -t 4 -I "$hb_ms" -P "$pidfile" &
    NODE_STARTED_PID=$!
    PIDS+=("$NODE_STARTED_PID")
    for _ in $(seq 1 50); do
        [ -s "$pidfile" ] && return 0
        sleep 0.05
    done
    return 1
}

echo "=== ForwardMesh ASAN Memory Tests (Protocol V2 + Mesh) ==="

# Test 1: Single-node push & pull
echo "[Test 1] Single-node push & pull with ASAN..."
N1="$WORK_ROOT/n1"
SRC="$WORK_ROOT/src.txt"
PULLED="$WORK_ROOT/pulled.txt"
mkdir -p "$N1"
printf "asan test payload\n" > "$SRC"

start_asan_node "$N1" $((BASE_PORT + 1)) 200 ""
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

start_asan_node "$C2" $((BASE_PORT + 12)) 150 ""; PC2="$NODE_STARTED_PID"
start_asan_node "$C1" $((BASE_PORT + 11)) 150 ""; PC1="$NODE_STARTED_PID"

"$ASAN_BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 11)),127.0.0.1:$((BASE_PORT + 12)) -f "$WORK_ROOT/blob.bin"
cmp -s "$WORK_ROOT/blob.bin" "$C2/blob.bin"
[ ! -e "$C1/blob.bin" ]

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

# Test 3: Mesh route learning, heartbeat sync, auto-completion & diamond failover
echo "[Test 3] Mesh auto-routing, heartbeat sync & failover under ASAN..."
M1="$WORK_ROOT/m1"
M2A="$WORK_ROOT/m2a"
M2B="$WORK_ROOT/m2b"
M3="$WORK_ROOT/m3"
PM1=$((BASE_PORT + 21))
PM2A=$((BASE_PORT + 22))
PM2B=$((BASE_PORT + 23))
PM3=$((BASE_PORT + 24))

start_asan_node "$M3" "$PM3" 120 ""; PID_M3="$NODE_STARTED_PID"
start_asan_node "$M2A" "$PM2A" 120 ""; PID_M2A="$NODE_STARTED_PID"
start_asan_node "$M2B" "$PM2B" 120 ""; PID_M2B="$NODE_STARTED_PID"
start_asan_node "$M1" "$PM1" 120 "127.0.0.1:$PM3"; PID_M1="$NODE_STARTED_PID"

"$ASAN_BIN_DIR/fwd" -a "127.0.0.1:$PM1,127.0.0.1:$PM2A,127.0.0.1:$PM3" -f "$SRC"
"$ASAN_BIN_DIR/fwd" -a "127.0.0.1:$PM1,127.0.0.1:$PM2B,127.0.0.1:$PM3" -f "$SRC"
sleep 0.3
"$ASAN_BIN_DIR/fwd" -a "127.0.0.1:$PM1" -l | grep -q "127.0.0.1:$PM3"

# Missing-hop completion
"$ASAN_BIN_DIR/fwd" -a "127.0.0.1:$PM1,127.0.0.1:$PM3" -f "$WORK_ROOT/blob.bin"
cmp -s "$WORK_ROOT/blob.bin" "$M3/blob.bin"

# Failover around stopped M2A
kill "$PID_M2A" && wait "$PID_M2A"
printf "asan failover\n" > "$WORK_ROOT/failover.txt"
"$ASAN_BIN_DIR/fwd" -a "127.0.0.1:$PM1,127.0.0.1:$PM2A,127.0.0.1:$PM3" -f "$WORK_ROOT/failover.txt"
cmp -s "$WORK_ROOT/failover.txt" "$M3/failover.txt"

kill "$PID_M1" "$PID_M2B" "$PID_M3"
wait "$PID_M1" "$PID_M2B" "$PID_M3"
echo "  PASS: Mesh auto-routing & failover clean under ASAN"

echo ""
echo "=== All ASAN tests passed (no memory errors detected) ==="
