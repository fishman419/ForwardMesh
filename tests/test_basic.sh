#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BIN_DIR="$PROJECT_DIR/bin"

WORK_ROOT="$(mktemp -d)"
BASE_PORT=$(( 42000 + ($$ % 4000) ))
PIDS=()

cleanup() {
    local status=$?
    if [ "$status" -ne 0 ]; then
        echo "=== Failure diagnostics ==="
        for logf in "$WORK_ROOT"/*/forward_server.log; do
            if [ -f "$logf" ]; then
                echo "--- $logf ---"
                cat "$logf"
            fi
        done
        if [ -f "$PROJECT_DIR/forward_client.log" ]; then
            echo "--- tail forward_client.log ---"
            tail -n 20 "$PROJECT_DIR/forward_client.log"
        fi
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

start_node() {
    local dir="$1"
    local port="$2"
    shift 2
    local pidfile="$dir/fwdd.pid"
    mkdir -p "$dir"
    "$BIN_DIR/fwdd" -F -d "$dir" -p "$port" -t 4 -P "$pidfile" "$@" &
    NODE_STARTED_PID=$!
    PIDS+=("$NODE_STARTED_PID")
    for _ in $(seq 1 50); do
        [ -s "$pidfile" ] && return 0
        if ! kill -0 "$NODE_STARTED_PID" 2>/dev/null; then
            echo "  FAIL: fwdd on port $port exited prematurely"
            return 1
        fi
        sleep 0.05
    done
    echo "  FAIL: fwdd on port $port timed out starting"
    return 1
}

stop_node() {
    for pid in "$@"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
}

echo "=== ForwardMesh Integration Tests (Protocol V2) ==="

# Test 1: Build
echo "[Test 1] Building..."
cd "$PROJECT_DIR"
rm -rf "$PROJECT_DIR/build" "$PROJECT_DIR/bin"
make > /dev/null
if [ -x "$BIN_DIR/fwdd" ] && [ -x "$BIN_DIR/fwd" ]; then
    echo "  PASS: Build successful"
else
    echo "  FAIL: Binary not found"
    exit 1
fi

# Test 2: CLI Help & Argument Validation
echo "[Test 2] CLI help & invalid argument validation..."
"$BIN_DIR/fwdd" -h | grep -q "Usage:"
"$BIN_DIR/fwd" -h | grep -q "Usage:"
if "$BIN_DIR/fwd" -a "not_an_ip:40000" -f /etc/hosts > /dev/null 2>&1; then
    echo "  FAIL: Invalid IP was accepted"
    exit 1
fi
if "$BIN_DIR/fwdd" -p 70000 > /dev/null 2>&1; then
    echo "  FAIL: Invalid port was accepted"
    exit 1
fi
echo "  PASS: CLI help and validation work"

# Test 3: Single-node Push (overwrite truncation + 0-byte empty file)
echo "[Test 3] Single-node push, overwrite truncation & empty file..."
NODE_DIR="$WORK_ROOT/t3_node"
SRC_DIR="$WORK_ROOT/t3_src"
mkdir -p "$NODE_DIR" "$SRC_DIR"
printf "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_OLD_TRAILING_DATA\n" > "$NODE_DIR/push_test.txt"
printf "short new content\n" > "$SRC_DIR/push_test.txt"
: > "$SRC_DIR/empty.txt"

start_node "$NODE_DIR" $((BASE_PORT + 1))
NODE_PID="$NODE_STARTED_PID"

"$BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 1)) -f "$SRC_DIR/push_test.txt"
cmp -s "$SRC_DIR/push_test.txt" "$NODE_DIR/push_test.txt"

"$BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 1)) -f "$SRC_DIR/empty.txt"
cmp -s "$SRC_DIR/empty.txt" "$NODE_DIR/empty.txt"

stop_node "$NODE_PID"
echo "  PASS: Single-node push, overwrite & empty file work"

# Test 4: Single-node Pull (including relative subdirectory)
echo "[Test 4] Single-node pull..."
NODE_DIR="$WORK_ROOT/t4_node"
OUT_DIR="$WORK_ROOT/t4_out"
mkdir -p "$NODE_DIR/subdir" "$OUT_DIR"
printf "pull content from subdir\n" > "$NODE_DIR/subdir/remote_file.txt"

start_node "$NODE_DIR" $((BASE_PORT + 2))
NODE_PID="$NODE_STARTED_PID"

"$BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 2)) -g subdir/remote_file.txt -f "$OUT_DIR/pulled.txt"
cmp -s "$NODE_DIR/subdir/remote_file.txt" "$OUT_DIR/pulled.txt"
stop_node "$NODE_PID"
echo "  PASS: Single-node pull works"

# Test 5: Three-node Cut-Through Streaming (zero disk writes on intermediate hops)
echo "[Test 5] Three-node cut-through streaming push & pull (256 KB)..."
N1="$WORK_ROOT/t5_n1"
N2="$WORK_ROOT/t5_n2"
N3="$WORK_ROOT/t5_n3"
SRC_BIN="$WORK_ROOT/t5_stream.bin"
PULLED_BIN="$WORK_ROOT/t5_pulled.bin"
dd if=/dev/urandom of="$SRC_BIN" bs=1024 count=256 2>/dev/null

start_node "$N3" $((BASE_PORT + 13)); P3="$NODE_STARTED_PID"
start_node "$N2" $((BASE_PORT + 12)); P2="$NODE_STARTED_PID"
start_node "$N1" $((BASE_PORT + 11)); P1="$NODE_STARTED_PID"

"$BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 11)),127.0.0.1:$((BASE_PORT + 12)),127.0.0.1:$((BASE_PORT + 13)) -f "$SRC_BIN"
cmp -s "$SRC_BIN" "$N3/t5_stream.bin"
if [ -e "$N1/t5_stream.bin" ] || [ -e "$N2/t5_stream.bin" ]; then
    echo "  FAIL: Intermediate hop wrote file during cut-through streaming"
    exit 1
fi

"$BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 11)),127.0.0.1:$((BASE_PORT + 12)),127.0.0.1:$((BASE_PORT + 13)) -g t5_stream.bin -f "$PULLED_BIN"
cmp -s "$SRC_BIN" "$PULLED_BIN"
if [ -e "$N1/t5_stream.bin" ] || [ -e "$N2/t5_stream.bin" ]; then
    echo "  FAIL: Intermediate hop wrote file during cut-through pull"
    exit 1
fi

stop_node "$P1" "$P2" "$P3"
echo "  PASS: Cut-through zero-disk streaming push & pull work"

# Test 6: Three-node Single-Pass Tee Store-and-Forward (-s flag)
echo "[Test 6] Three-node tee caching push & pull (-s flag, 256 KB)..."
N1="$WORK_ROOT/t6_n1"
N2="$WORK_ROOT/t6_n2"
N3="$WORK_ROOT/t6_n3"
mkdir -p "$N1" "$N2" "$N3"
SRC_CACHE="$WORK_ROOT/t6_cached.bin"
dd if=/dev/urandom of="$SRC_CACHE" bs=1024 count=256 2>/dev/null
dd if=/dev/urandom of="$N3/remote_cached.bin" bs=1024 count=256 2>/dev/null

start_node "$N3" $((BASE_PORT + 23)); P3="$NODE_STARTED_PID"
start_node "$N2" $((BASE_PORT + 22)); P2="$NODE_STARTED_PID"
start_node "$N1" $((BASE_PORT + 21)); P1="$NODE_STARTED_PID"

"$BIN_DIR/fwd" -s -a 127.0.0.1:$((BASE_PORT + 21)),127.0.0.1:$((BASE_PORT + 22)),127.0.0.1:$((BASE_PORT + 23)) -f "$SRC_CACHE"
cmp -s "$SRC_CACHE" "$N1/t6_cached.bin"
cmp -s "$SRC_CACHE" "$N2/t6_cached.bin"
cmp -s "$SRC_CACHE" "$N3/t6_cached.bin"

"$BIN_DIR/fwd" -s -a 127.0.0.1:$((BASE_PORT + 21)),127.0.0.1:$((BASE_PORT + 22)),127.0.0.1:$((BASE_PORT + 23)) -g remote_cached.bin -f "$WORK_ROOT/t6_pulled.bin"
cmp -s "$N3/remote_cached.bin" "$WORK_ROOT/t6_pulled.bin"
cmp -s "$N3/remote_cached.bin" "$N1/remote_cached.bin"
cmp -s "$N3/remote_cached.bin" "$N2/remote_cached.bin"

stop_node "$P1" "$P2" "$P3"
echo "  PASS: Single-pass tee caching (-s) works for push & pull"

# Test 7: Concurrent Multi-Client Push & Pull
echo "[Test 7] Concurrent requests across 2-node chain..."
N1="$WORK_ROOT/t7_n1"
N2="$WORK_ROOT/t7_n2"
CONC_DIR="$WORK_ROOT/t7_clients"
mkdir -p "$N1" "$N2" "$CONC_DIR"

start_node "$N2" $((BASE_PORT + 32)); P2="$NODE_STARTED_PID"
start_node "$N1" $((BASE_PORT + 31)); P1="$NODE_STARTED_PID"

CLIENT_PIDS=()
for i in $(seq 1 8); do
    printf "concurrent payload %d\n" "$i" > "$CONC_DIR/push_${i}.txt"
    printf "remote concurrent %d\n" "$i" > "$N2/pull_${i}.txt"
    "$BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 31)),127.0.0.1:$((BASE_PORT + 32)) -f "$CONC_DIR/push_${i}.txt" &
    CLIENT_PIDS+=("$!")
    "$BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 31)),127.0.0.1:$((BASE_PORT + 32)) -g "pull_${i}.txt" -f "$CONC_DIR/got_${i}.txt" &
    CLIENT_PIDS+=("$!")
done

for cpid in "${CLIENT_PIDS[@]}"; do
    wait "$cpid"
done

for i in $(seq 1 8); do
    cmp -s "$CONC_DIR/push_${i}.txt" "$N2/push_${i}.txt"
    cmp -s "$N2/pull_${i}.txt" "$CONC_DIR/got_${i}.txt"
done
stop_node "$P1" "$P2"
echo "  PASS: Concurrent push & pull succeeded"

# Test 8: Error Handling, Path Traversal & CRC32 Corruption Rejection
echo "[Test 8] Error propagation, path traversal & CRC32 corruption rejection..."
N1="$WORK_ROOT/t8_n1"
mkdir -p "$N1"
printf "secret outside\n" > "$WORK_ROOT/outside_secret.txt"

start_node "$N1" $((BASE_PORT + 41)); P1="$NODE_STARTED_PID"

if "$BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 41)) -g missing_file.txt -f "$WORK_ROOT/out_missing.txt" > /dev/null 2>&1; then
    echo "  FAIL: Pulling missing file should return non-zero"
    exit 1
fi

printf "hello\n" > "$WORK_ROOT/t8_hello.txt"
if "$BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 41)),127.0.0.1:$((BASE_PORT + 99)) -f "$WORK_ROOT/t8_hello.txt" > /dev/null 2>&1; then
    echo "  FAIL: Pushing to unreachable next hop should return non-zero"
    exit 1
fi

if "$BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 41)) -g ../outside_secret.txt -f "$WORK_ROOT/stolen.txt" > /dev/null 2>&1; then
    echo "  FAIL: Path traversal should be rejected"
    exit 1
fi

# Inject tampered payload with bad CRC32 over raw TCP and assert retcode == 6 (ForwardChecksumMismatch)
if command -v python3 > /dev/null 2>&1; then
    python3 - "$((BASE_PORT + 41))" << 'PYEOF'
import socket, struct, sys

port = int(sys.argv[1])
fname = b"corrupted.txt\x00"
payload = b"tampered payload bytes"
# ForwardRequest: uint64 length, uint32 magic, uint8 ver, cmd, ttl, flags, uint64 id, uint32 bad_crc32
total_len = 28 + 4 + len(fname) + len(payload)
hdr = struct.pack("!QIBBBBQI", total_len, 0xFF44AADD, 2, 1, 0, 0, 0x1234, 0xDEADBEEF)
fmeta = struct.pack("!I", len(fname)) + fname

with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
    s.sendall(hdr + fmeta + payload)
    resp = s.recv(28)
    assert len(resp) == 28, f"short resp {len(resp)}"
    r_len, r_magic, r_ver, r_cmd, r_ttl, r_retcode, r_id, r_crc = struct.unpack("!QIBBBBQI", resp)
    assert r_magic == 0xFF44AADD and r_retcode == 6, f"expected ForwardChecksumMismatch(6), got {r_retcode}"
PYEOF
    if [ -e "$N1/corrupted.txt" ]; then
        echo "  FAIL: Corrupted payload should never be saved to disk"
        exit 1
    fi
fi

"$BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 41)) -f "$WORK_ROOT/t8_hello.txt"
cmp -s "$WORK_ROOT/t8_hello.txt" "$N1/t8_hello.txt"
stop_node "$P1"
echo "  PASS: Error handling, path sandbox, and CRC32 rejection work"

# Test 9: Daemon Mode + PID File + Graceful SIGTERM
echo "[Test 9] Daemon mode & PID file lifecycle..."
DAEMON_DIR="$WORK_ROOT/t9_daemon"
PID_FILE="$WORK_ROOT/fwdd.pid"
mkdir -p "$DAEMON_DIR"
"$BIN_DIR/fwdd" -d "$DAEMON_DIR" -p $((BASE_PORT + 51)) -P "$PID_FILE"

for _ in $(seq 1 50); do
    [ -s "$PID_FILE" ] && break
    sleep 0.05
done
DAEMON_PID="$(cat "$PID_FILE")"
PIDS+=("$DAEMON_PID")

"$BIN_DIR/fwd" -a 127.0.0.1:$((BASE_PORT + 51)) -f "$WORK_ROOT/t8_hello.txt"
cmp -s "$WORK_ROOT/t8_hello.txt" "$DAEMON_DIR/t8_hello.txt"

kill "$DAEMON_PID"
for _ in $(seq 1 50); do
    kill -0 "$DAEMON_PID" 2>/dev/null || break
    sleep 0.05
done
if [ -e "$PID_FILE" ]; then
    echo "  FAIL: PID file was not removed on clean shutdown"
    exit 1
fi
echo "  PASS: Daemon mode and PID file lifecycle work"

echo ""
echo "=== All tests passed ==="
