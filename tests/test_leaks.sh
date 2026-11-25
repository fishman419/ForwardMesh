#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LEAKS_BIN_DIR="${LEAKS_BIN:-$PROJECT_DIR/bin_leaks}"

WORK_ROOT="$(mktemp -d)"
PIDS=()

cleanup() {
    for pid in "${PIDS[@]:-}"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$WORK_ROOT"
}
trap cleanup EXIT

echo "=== ForwardMesh Memory Leak Tests (macOS leaks) ==="

if ! command -v leaks > /dev/null 2>&1; then
    echo "SKIP: macOS leaks(1) utility not found"
    exit 0
fi

N1="$WORK_ROOT/n1"
N2="$WORK_ROOT/n2"
mkdir -p "$N1" "$N2"
printf "leak check payload\n" > "$WORK_ROOT/sample.txt"

"$LEAKS_BIN_DIR/fwdd" -F -d "$N2" -p 41302 -t 4 -P "$N2/fwdd.pid" &
P2=$!
PIDS+=("$P2")
"$LEAKS_BIN_DIR/fwdd" -F -d "$N1" -p 41301 -t 4 -P "$N1/fwdd.pid" &
P1=$!
PIDS+=("$P1")

for _ in $(seq 1 50); do
    [ -s "$N1/fwdd.pid" ] && [ -s "$N2/fwdd.pid" ] && break
    sleep 0.05
done

echo "[Test 1] Exercising push, pull, and error paths..."
"$LEAKS_BIN_DIR/fwd" -a 127.0.0.1:41301,127.0.0.1:41302 -f "$WORK_ROOT/sample.txt"
"$LEAKS_BIN_DIR/fwd" -a 127.0.0.1:41301,127.0.0.1:41302 -g sample.txt -f "$WORK_ROOT/pulled.txt"
cmp -s "$WORK_ROOT/sample.txt" "$WORK_ROOT/pulled.txt"
"$LEAKS_BIN_DIR/fwd" -a 127.0.0.1:41301,127.0.0.1:41302 -g missing.txt -f "$WORK_ROOT/none.txt" > /dev/null 2>&1 || true

echo "[Test 2] Inspecting live daemon processes with leaks(1)..."
for pid in "$P1" "$P2"; do
    set +e
    LEAK_OUT="$(leaks -q "$pid" 2>&1)"
    LEAK_RC=$?
    set -e
    if [ "$LEAK_RC" -eq 1 ]; then
        echo "  FAIL: Memory leaks detected in PID $pid:"
        echo "$LEAK_OUT"
        exit 1
    elif [ "$LEAK_RC" -ne 0 ]; then
        echo "  WARN: leaks(1) could not attach to PID $pid (rc=$LEAK_RC): $LEAK_OUT"
    else
        echo "  PASS: No leaks in PID $pid"
    fi
done

kill "$P1" "$P2"
wait "$P1" "$P2"
echo ""
echo "=== Leak tests completed ==="
