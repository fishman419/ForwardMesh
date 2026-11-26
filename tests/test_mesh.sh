#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BIN_DIR="$PROJECT_DIR/bin"

WORK_ROOT="$(mktemp -d)"
BASE_PORT=$(( 43000 + ($$ % 3000) ))
PIDS=()

cleanup() {
    local status=$?
    if [ "$status" -ne 0 ]; then
        echo "=== Mesh Test Failure Diagnostics ==="
        for logf in "$WORK_ROOT"/*/forward_server.log; do
            if [ -f "$logf" ]; then
                echo "--- $logf ---"
                tail -n 30 "$logf"
            fi
        done
        if [ -f "$PROJECT_DIR/forward_client.log" ]; then
            echo "--- tail forward_client.log ---"
            tail -n 25 "$PROJECT_DIR/forward_client.log"
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

start_mesh_node() {
    local dir="$1"
    local port="$2"
    local hb_ms="$3"
    local deny_peers="${4:-}"
    local pidfile="$dir/fwdd.pid"
    mkdir -p "$dir"
    rm -f "$pidfile"
    FWDD_DENY_PEERS="$deny_peers" "$BIN_DIR/fwdd" -F -d "$dir" -p "$port" -t 8 -I "$hb_ms" -P "$pidfile" &
    NODE_STARTED_PID=$!
    PIDS+=("$NODE_STARTED_PID")
    for _ in $(seq 1 60); do
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

echo "=== ForwardMesh Auto-Routing & Failover Tests ==="

# IT-01 & IT-02: Baseline Isolation, Self-Learning, and Missing-Hop Completion
echo "[IT-01 & IT-02] Cross-subnet isolation, self-learning & missing-hop completion..."
P1=$((BASE_PORT + 1))
P2=$((BASE_PORT + 2))
P3=$((BASE_PORT + 3))
D1="$WORK_ROOT/it1_n1"
D2="$WORK_ROOT/it1_n2"
D3="$WORK_ROOT/it1_n3"
SRC_DIR="$WORK_ROOT/it1_src"
mkdir -p "$SRC_DIR" "$D3"

start_mesh_node "$D3" "$P3" 200 ""; PID_D3="$NODE_STARTED_PID"
start_mesh_node "$D2" "$P2" 200 ""; PID_D2="$NODE_STARTED_PID"
# N1 cannot directly reach N3 (simulated cross-subnet firewall)
start_mesh_node "$D1" "$P1" 200 "127.0.0.1:$P3"; PID_D1="$NODE_STARTED_PID"

# Empty route table query works cleanly
"$BIN_DIR/fwd" -a "127.0.0.1:$P1" -l | grep -q "no routes learned"

printf "initial seed\n" > "$SRC_DIR/init.txt"
# Before learning, N1 -> N3 abbreviated push must fail due to subnet isolation
if "$BIN_DIR/fwd" -a "127.0.0.1:$P1,127.0.0.1:$P3" -f "$SRC_DIR/init.txt" > /dev/null 2>&1; then
    echo "  FAIL: Expected unlearned cross-subnet direct hop to fail"
    exit 1
fi

# First full-chain transfer teaches N1 and N2
"$BIN_DIR/fwd" -a "127.0.0.1:$P1,127.0.0.1:$P2,127.0.0.1:$P3" -f "$SRC_DIR/init.txt"
cmp -s "$SRC_DIR/init.txt" "$D3/init.txt"
"$BIN_DIR/fwd" -a "127.0.0.1:$P1" -l | grep -q "127.0.0.1:$P3"

# Abbreviated Push (256 KB, omitting N2): zero disk on N2, delivered to N3
dd if=/dev/urandom of="$SRC_DIR/push256.bin" bs=1024 count=256 2>/dev/null
"$BIN_DIR/fwd" -a "127.0.0.1:$P1,127.0.0.1:$P3" -f "$SRC_DIR/push256.bin"
cmp -s "$SRC_DIR/push256.bin" "$D3/push256.bin"
[ ! -e "$D2/push256.bin" ]

# Abbreviated Push with -s tee caching: cached on auto-inserted N2 and N3
dd if=/dev/urandom of="$SRC_DIR/cache128.bin" bs=1024 count=128 2>/dev/null
"$BIN_DIR/fwd" -s -a "127.0.0.1:$P1,127.0.0.1:$P3" -f "$SRC_DIR/cache128.bin"
cmp -s "$SRC_DIR/cache128.bin" "$D2/cache128.bin"
cmp -s "$SRC_DIR/cache128.bin" "$D3/cache128.bin"

# Abbreviated Pull (omitting N2)
dd if=/dev/urandom of="$D3/remote_pull.bin" bs=1024 count=128 2>/dev/null
"$BIN_DIR/fwd" -a "127.0.0.1:$P1,127.0.0.1:$P3" -g remote_pull.bin -f "$SRC_DIR/pulled.bin"
cmp -s "$D3/remote_pull.bin" "$SRC_DIR/pulled.bin"

stop_node "$PID_D1" "$PID_D2" "$PID_D3"
echo "  PASS: IT-01 & IT-02 passed"

# IT-03: 4-Node 3-Subnet Segmented Transfers Stitched via Heartbeat Sync
echo "[IT-03] 4-node segmented learning & heartbeat path-vector convergence..."
Q1=$((BASE_PORT + 11))
Q2=$((BASE_PORT + 12))
Q3=$((BASE_PORT + 13))
Q4=$((BASE_PORT + 14))
S1="$WORK_ROOT/it3_n1"
S2="$WORK_ROOT/it3_n2"
S3="$WORK_ROOT/it3_n3"
S4="$WORK_ROOT/it3_n4"

start_mesh_node "$S4" "$Q4" 150 ""; PID_S4="$NODE_STARTED_PID"
start_mesh_node "$S3" "$Q3" 150 ""; PID_S3="$NODE_STARTED_PID"
start_mesh_node "$S2" "$Q2" 150 "127.0.0.1:$Q4"; PID_S2="$NODE_STARTED_PID"
start_mesh_node "$S1" "$Q1" 150 "127.0.0.1:$Q3,127.0.0.1:$Q4"; PID_S1="$NODE_STARTED_PID"

printf "seg12\n" > "$WORK_ROOT/seg.txt"
"$BIN_DIR/fwd" -a "127.0.0.1:$Q1,127.0.0.1:$Q2" -f "$WORK_ROOT/seg.txt"
"$BIN_DIR/fwd" -a "127.0.0.1:$Q2,127.0.0.1:$Q3" -f "$WORK_ROOT/seg.txt"
"$BIN_DIR/fwd" -a "127.0.0.1:$Q3,127.0.0.1:$Q4" -f "$WORK_ROOT/seg.txt"

CONVERGED=0
for _ in $(seq 1 30); do
    if "$BIN_DIR/fwd" -a "127.0.0.1:$Q1" -l | grep -q "127.0.0.1:$Q4"; then
        CONVERGED=1
        break
    fi
    sleep 0.1
done
if [ "$CONVERGED" -ne 1 ]; then
    echo "  FAIL: N1 did not converge 3-hop route to N4 via heartbeat"
    exit 1
fi

dd if=/dev/urandom of="$WORK_ROOT/hop4.bin" bs=1024 count=64 2>/dev/null
"$BIN_DIR/fwd" -a "127.0.0.1:$Q1,127.0.0.1:$Q4" -f "$WORK_ROOT/hop4.bin"
cmp -s "$WORK_ROOT/hop4.bin" "$S4/hop4.bin"
stop_node "$PID_S1" "$PID_S2" "$PID_S3" "$PID_S4"
echo "  PASS: IT-03 passed"

# IT-04: On-Demand Immediate Sync When Periodic Heartbeat Is Disabled (-I 0)
echo "[IT-04] On-demand neighbor route sync (-I 0)..."
O1=$((BASE_PORT + 21))
O2=$((BASE_PORT + 22))
O3=$((BASE_PORT + 23))
OD1="$WORK_ROOT/it4_n1"
OD2="$WORK_ROOT/it4_n2"
OD3="$WORK_ROOT/it4_n3"

start_mesh_node "$OD3" "$O3" 0 ""; PID_OD3="$NODE_STARTED_PID"
start_mesh_node "$OD2" "$O2" 0 ""; PID_OD2="$NODE_STARTED_PID"
start_mesh_node "$OD1" "$O1" 0 "127.0.0.1:$O3"; PID_OD1="$NODE_STARTED_PID"

printf "step1\n" > "$WORK_ROOT/od_step.txt"
"$BIN_DIR/fwd" -a "127.0.0.1:$O1,127.0.0.1:$O2" -f "$WORK_ROOT/od_step.txt"
"$BIN_DIR/fwd" -a "127.0.0.1:$O2,127.0.0.1:$O3" -f "$WORK_ROOT/od_step.txt"

printf "instant on-demand payload\n" > "$WORK_ROOT/ondemand.txt"
"$BIN_DIR/fwd" -a "127.0.0.1:$O1,127.0.0.1:$O3" -f "$WORK_ROOT/ondemand.txt"
cmp -s "$WORK_ROOT/ondemand.txt" "$OD3/ondemand.txt"
stop_node "$PID_OD1" "$PID_OD2" "$PID_OD3"
echo "  PASS: IT-04 passed"

# IT-05, IT-06, IT-07: Diamond Failover, Remote Degradation, Recovery, Strict -R & Persistence
echo "[IT-05/06/07] Diamond failover, remote link degradation, strict -R & persistence..."
M1=$((BASE_PORT + 31))
M2A=$((BASE_PORT + 32))
M2B=$((BASE_PORT + 33))
M3=$((BASE_PORT + 34))
MD1="$WORK_ROOT/it5_n1"
MD2A="$WORK_ROOT/it5_n2a"
MD2B="$WORK_ROOT/it5_n2b"
MD3="$WORK_ROOT/it5_n3"

start_mesh_node "$MD3" "$M3" 150 ""; PID_M3="$NODE_STARTED_PID"
start_mesh_node "$MD2A" "$M2A" 150 ""; PID_M2A="$NODE_STARTED_PID"
start_mesh_node "$MD2B" "$M2B" 150 ""; PID_M2B="$NODE_STARTED_PID"
start_mesh_node "$MD1" "$M1" 150 "127.0.0.1:$M3"; PID_M1="$NODE_STARTED_PID"

printf "seed paths\n" > "$WORK_ROOT/m_seed.txt"
printf "remote m3 data\n" > "$MD3/m3_pull.txt"
"$BIN_DIR/fwd" -a "127.0.0.1:$M1,127.0.0.1:$M2A,127.0.0.1:$M3" -f "$WORK_ROOT/m_seed.txt"
"$BIN_DIR/fwd" -a "127.0.0.1:$M1,127.0.0.1:$M2B,127.0.0.1:$M3" -f "$WORK_ROOT/m_seed.txt"

# Kill N2A; explicit chain naming dead N2A must reroute via N2B for Push & Pull
stop_node "$PID_M2A"
printf "reroute push payload\n" > "$WORK_ROOT/reroute_push.txt"
"$BIN_DIR/fwd" -a "127.0.0.1:$M1,127.0.0.1:$M2A,127.0.0.1:$M3" -f "$WORK_ROOT/reroute_push.txt"
cmp -s "$WORK_ROOT/reroute_push.txt" "$MD3/reroute_push.txt"

"$BIN_DIR/fwd" -a "127.0.0.1:$M1,127.0.0.1:$M2A,127.0.0.1:$M3" -g m3_pull.txt -f "$WORK_ROOT/reroute_pull.txt"
cmp -s "$MD3/m3_pull.txt" "$WORK_ROOT/reroute_pull.txt"

# IT-06: Strict -R mode must reject missing hop AND reject rerouting around dead N2A
if "$BIN_DIR/fwd" -R -a "127.0.0.1:$M1,127.0.0.1:$M3" -f "$WORK_ROOT/m_seed.txt" > /dev/null 2>&1; then
    echo "  FAIL: Strict -R allowed missing-hop auto-completion"
    exit 1
fi
if "$BIN_DIR/fwd" -R -a "127.0.0.1:$M1,127.0.0.1:$M2A,127.0.0.1:$M3" -f "$WORK_ROOT/m_seed.txt" > /dev/null 2>&1; then
    echo "  FAIL: Strict -R rerouted around dead N2A"
    exit 1
fi
printf "strict valid\n" > "$WORK_ROOT/strict_ok.txt"
"$BIN_DIR/fwd" -R -a "127.0.0.1:$M1,127.0.0.1:$M2B,127.0.0.1:$M3" -f "$WORK_ROOT/strict_ok.txt"
cmp -s "$WORK_ROOT/strict_ok.txt" "$MD3/strict_ok.txt"

# Revive N2A and stop N2B -> heartbeat restores N2A and traffic switches back
start_mesh_node "$MD2A" "$M2A" 150 ""; PID_M2A="$NODE_STARTED_PID"
stop_node "$PID_M2B"
sleep 0.45
printf "recovered via n2a\n" > "$WORK_ROOT/recovered.txt"
"$BIN_DIR/fwd" -a "127.0.0.1:$M1,127.0.0.1:$M3" -f "$WORK_ROOT/recovered.txt"
cmp -s "$WORK_ROOT/recovered.txt" "$MD3/recovered.txt"

# IT-07: Restart M1 and verify persisted mesh_routes.dat works without re-teaching
stop_node "$PID_M1"
if [ ! -s "$MD1/mesh_routes.dat" ]; then
    echo "  FAIL: mesh_routes.dat was not persisted"
    exit 1
fi
start_mesh_node "$MD1" "$M1" 150 "127.0.0.1:$M3"; PID_M1="$NODE_STARTED_PID"
printf "after m1 restart\n" > "$WORK_ROOT/persisted.txt"
"$BIN_DIR/fwd" -a "127.0.0.1:$M1,127.0.0.1:$M3" -f "$WORK_ROOT/persisted.txt"
cmp -s "$WORK_ROOT/persisted.txt" "$MD3/persisted.txt"
stop_node "$PID_M1" "$PID_M2A" "$PID_M3"

# IT-05c: Remote sub-hop failure behind a still-alive neighbor
R1=$((BASE_PORT + 41))
R2A=$((BASE_PORT + 42))
R2SUB=$((BASE_PORT + 43))
R2B=$((BASE_PORT + 44))
R3=$((BASE_PORT + 45))
RD1="$WORK_ROOT/it5c_n1"
RD2A="$WORK_ROOT/it5c_n2a"
RD2SUB="$WORK_ROOT/it5c_n2sub"
RD2B="$WORK_ROOT/it5c_n2b"
RD3="$WORK_ROOT/it5c_n3"

start_mesh_node "$RD3" "$R3" 120 ""; PID_R3="$NODE_STARTED_PID"
start_mesh_node "$RD2SUB" "$R2SUB" 120 ""; PID_R2SUB="$NODE_STARTED_PID"
start_mesh_node "$RD2A" "$R2A" 120 "127.0.0.1:$R3"; PID_R2A="$NODE_STARTED_PID"
start_mesh_node "$RD2B" "$R2B" 120 ""; PID_R2B="$NODE_STARTED_PID"
start_mesh_node "$RD1" "$R1" 120 "127.0.0.1:$R2SUB,127.0.0.1:$R3"; PID_R1="$NODE_STARTED_PID"

"$BIN_DIR/fwd" -R -a "127.0.0.1:$R1,127.0.0.1:$R2A,127.0.0.1:$R2SUB,127.0.0.1:$R3" -f "$WORK_ROOT/m_seed.txt"
"$BIN_DIR/fwd" -R -a "127.0.0.1:$R1,127.0.0.1:$R2B,127.0.0.1:$R3" -f "$WORK_ROOT/m_seed.txt"
# Kill R2SUB while R2A remains alive; wait for heartbeat degradation
stop_node "$PID_R2SUB"
sleep 0.45
printf "remote subhop failover\n" > "$WORK_ROOT/sub_failover.txt"
"$BIN_DIR/fwd" -a "127.0.0.1:$R1,127.0.0.1:$R3" -f "$WORK_ROOT/sub_failover.txt"
cmp -s "$WORK_ROOT/sub_failover.txt" "$RD3/sub_failover.txt"
stop_node "$PID_R1" "$PID_R2A" "$PID_R2B" "$PID_R3"
echo "  PASS: IT-05, IT-06 & IT-07 passed"

# FT-01 & FT-02: 32-Hop Loop Ceiling & Malformed Sync Packet Injection
echo "[FT-01 & FT-02] 32-hop loop ceiling & malformed sync packet rejection..."
L1=$((BASE_PORT + 51))
L2=$((BASE_PORT + 52))
GHOST=$((BASE_PORT + 59))
LD1="$WORK_ROOT/ft1_l1"
LD2="$WORK_ROOT/ft1_l2"
mkdir -p "$LD1" "$LD2"

cat > "$LD1/mesh_routes.dat" << EOF
# Static loop test
NEIGHBOR 127.0.0.1:$L2 1
ROUTE 127.0.0.1:$GHOST 2 127.0.0.1:$L2,127.0.0.1:$GHOST
EOF
cat > "$LD2/mesh_routes.dat" << EOF
# Static loop test
NEIGHBOR 127.0.0.1:$L1 1
ROUTE 127.0.0.1:$GHOST 2 127.0.0.1:$L1,127.0.0.1:$GHOST
EOF

start_mesh_node "$LD1" "$L1" 0 "127.0.0.1:$GHOST"; PID_L1="$NODE_STARTED_PID"
start_mesh_node "$LD2" "$L2" 0 "127.0.0.1:$GHOST"; PID_L2="$NODE_STARTED_PID"

if "$BIN_DIR/fwd" -a "127.0.0.1:$L1,127.0.0.1:$GHOST" -f "$WORK_ROOT/m_seed.txt" > /dev/null 2>&1; then
    echo "  FAIL: Static routing loop should terminate with non-zero error"
    exit 1
fi

# FT-02: Truncated (<28B) sync header, bogus-length sync request, and bad-CRC32 sync response
if command -v python3 > /dev/null 2>&1; then
    python3 - "$L1" << 'PYEOF'
import socket, struct, sys, threading

port = int(sys.argv[1])

# 1) Truncated 12-byte partial header + EOF
with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
    s.sendall(b"\x00\x00\x00\x00\x00\x00\x00\x1c\xff\x44\xaa\xdd")

# 2) cmd=2 (ForwardMeshSync) with bogus length=999 -> ForwardInvalidRequest(4)
hdr = struct.pack("!QIBBBBQI", 999, 0xFF44AADD, 2, 2, 0, 0, 0x7777, 0)
with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
    s.sendall(hdr)
    resp = s.recv(28)
    assert len(resp) == 28
    _, magic, ver, cmd, ttl, retcode, _, _ = struct.unpack("!QIBBBBQI", resp)
    assert magic == 0xFF44AADD and retcode == 4, f"expected 4, got {retcode}"
PYEOF

    # 3) Mock neighbor serving forged-CRC32 ForwardMeshSync response must be rejected
    MOCK_PORT=$((BASE_PORT + 58))
    MD_BAD="$WORK_ROOT/ft2_badsync"
    mkdir -p "$MD_BAD"
    cat > "$MD_BAD/mesh_routes.dat" << EOF
NEIGHBOR 127.0.0.1:$MOCK_PORT 1
EOF
    python3 - "$MOCK_PORT" << 'PYEOF' &
import socket, struct, sys
mport = int(sys.argv[1])
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", mport))
srv.listen(4)
srv.settimeout(5)
try:
    conn, _ = srv.accept()
    _ = conn.recv(28)
    poison_ip = socket.inet_aton("10.99.99.99")
    entry = poison_ip + struct.pack("!IHBB", 49999, 1, 1, 1) + poison_ip + struct.pack("!I", 49999)
    payload = struct.pack("!I", 1) + entry
    # Deliberately wrong crc32 = 0xDEADBEEF
    rhdr = struct.pack("!QIBBBBQI", 28 + len(payload), 0xFF44AADD, 2, 2, 0, 0, 0, 0xDEADBEEF)
    conn.sendall(rhdr + payload)
    conn.close()
except Exception:
    pass
finally:
    srv.close()
PYEOF
    MOCK_PID=$!
    sleep 0.1
    start_mesh_node "$MD_BAD" $((BASE_PORT + 57)) 5000 ""; PID_BAD="$NODE_STARTED_PID"
    wait "$MOCK_PID" 2>/dev/null || true
    if "$BIN_DIR/fwd" -a "127.0.0.1:$((BASE_PORT + 57))" -l | grep -q "10.99.99.99"; then
        echo "  FAIL: Forged-CRC32 sync route was accepted"
        exit 1
    fi
    stop_node "$PID_BAD"
fi

# Nodes remain healthy after loop breaker and malformed packet
"$BIN_DIR/fwd" -a "127.0.0.1:$L1,127.0.0.1:$L2" -f "$WORK_ROOT/m_seed.txt"
cmp -s "$WORK_ROOT/m_seed.txt" "$LD2/m_seed.txt"
stop_node "$PID_L1" "$PID_L2"
echo "  PASS: FT-01 & FT-02 passed"

# CT-01: High-Frequency Heartbeat (-I 50ms) + 24 Concurrent Mesh Clients
echo "[CT-01] High-frequency heartbeat (-I 50ms) + 24 concurrent clients..."
K1=$((BASE_PORT + 61))
K2=$((BASE_PORT + 62))
K3=$((BASE_PORT + 63))
KD1="$WORK_ROOT/ct_n1"
KD2="$WORK_ROOT/ct_n2"
KD3="$WORK_ROOT/ct_n3"
KCLI="$WORK_ROOT/ct_cli"
mkdir -p "$KCLI" "$KD3"

start_mesh_node "$KD3" "$K3" 50 ""; PID_K3="$NODE_STARTED_PID"
start_mesh_node "$KD2" "$K2" 50 ""; PID_K2="$NODE_STARTED_PID"
start_mesh_node "$KD1" "$K1" 50 "127.0.0.1:$K3"; PID_K1="$NODE_STARTED_PID"

"$BIN_DIR/fwd" -a "127.0.0.1:$K1,127.0.0.1:$K2,127.0.0.1:$K3" -f "$WORK_ROOT/m_seed.txt"

CPIDS=()
for i in $(seq 1 8); do
    printf "mesh conc push %d\n" "$i" > "$KCLI/mpush_${i}.txt"
    printf "mesh conc pull %d\n" "$i" > "$KD3/mpull_${i}.txt"
    "$BIN_DIR/fwd" -a "127.0.0.1:$K1,127.0.0.1:$K3" -f "$KCLI/mpush_${i}.txt" &
    CPIDS+=("$!")
    "$BIN_DIR/fwd" -a "127.0.0.1:$K1,127.0.0.1:$K3" -g "mpull_${i}.txt" -f "$KCLI/mgot_${i}.txt" &
    CPIDS+=("$!")
    "$BIN_DIR/fwd" -a "127.0.0.1:$K1" -l > /dev/null &
    CPIDS+=("$!")
done

for cpid in "${CPIDS[@]}"; do
    wait "$cpid"
done

for i in $(seq 1 8); do
    cmp -s "$KCLI/mpush_${i}.txt" "$KD3/mpush_${i}.txt"
    cmp -s "$KD3/mpull_${i}.txt" "$KCLI/mgot_${i}.txt"
done
stop_node "$PID_K1" "$PID_K2" "$PID_K3"
echo "  PASS: CT-01 passed"

echo ""
echo "=== All Mesh auto-routing & failover tests passed ==="
