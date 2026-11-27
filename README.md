# ForwardMesh

Cross-subnet multi-hop file forwarding mesh with zero-config route learning, path-vector heartbeat synchronization, automatic missing-hop completion, mid-path failover rerouting, cut-through streaming, and end-to-end CRC32 verification.

## Architecture

[![ForwardMesh Architecture](docs/architecture.svg)](docs/architecture.svg)

> Full editorial page (with Mesh control-plane, data-plane & Protocol V2 summary cards): [docs/architecture.html](https://htmlpreview.github.io/?https://github.com/fishman419/ForwardMesh/blob/main/docs/architecture.html)

## Build & Test

```bash
# Build release binaries (bin/fwdd, bin/fwd)
make

# Run C++ unit tests (MeshRouter & wire codec)
make unit-test

# Run full test suite (unit + Protocol V2 integration + cross-subnet Mesh suite)
make test

# Run AddressSanitizer tests
make asan-test

# Run macOS leaks(1) check
make leaks-check
```

## Run

**Server (`fwdd`):**
```text
./bin/fwdd [-p port] [-d dir] [-t threads] [-P pidfile] [-F] [-S] [-I heartbeat_ms] [-h]
  -p port         : specify port (default 40000)
  -d dir          : specify working directory (default .)
  -t threads      : number of base worker threads (default 4, scales elastically up to 64)
  -P pidfile      : write process PID to pidfile
  -F              : run in foreground (do not daemonize)
  -S              : always cache relayed files on intermediate hops
  -I heartbeat_ms : neighbor probe & route sync interval in ms (default 3000, 0 to disable)
  -h              : show this help
```

**Client (`fwd`):**
```text
./bin/fwd -a ip:port,... [options]
  -a addr : comma-separated list of ip:port forwarding chain
  -f file : file to send (push) or local output path (pull)
  -g file : remote relative file path to get (pull mode)
  -s      : cache file copies on intermediate hops (default: zero-disk streaming)
  -R      : strict source routing (disable Mesh route completion & failover)
  -l      : query and display Mesh routing table of the first node in -a
  -h      : show this help
```

## Examples

**1. Zero-Config Mesh Route Learning & Missing-Hop Completion:**
```bash
# First transfer across subnets teaches Node1 and Node2 the path to Node3:
./bin/fwd -a 10.0.1.1:40000,10.0.2.1:40000,192.168.50.2:40000 -f init.txt

# Inspect Node1's learned Mesh routing table:
./bin/fwd -a 10.0.1.1:40000 -l

# Subsequent Push or Pull can omit intermediate hops — Node1 auto-completes via Node2:
./bin/fwd -a 10.0.1.1:40000,192.168.50.2:40000 -f release.tar.zst
./bin/fwd -a 10.0.1.1:40000,192.168.50.2:40000 -g release.tar.zst -f ./downloaded.tar.zst
```

**2. Automatic Failover vs. Strict Source Routing (`-R`):**
```bash
# Default mode: if 10.0.2.1 is down and Node1 knows backup hop 10.0.3.1 -> 192.168.50.2,
# Node1 automatically reroutes around 10.0.2.1:
./bin/fwd -a 10.0.1.1:40000,10.0.2.1:40000,192.168.50.2:40000 -f data.bin

# Strict mode (-R): follow the exact -a hop list only; fail immediately if any hop is unreachable:
./bin/fwd -R -a 10.0.1.1:40000,10.0.2.1:40000,192.168.50.2:40000 -f data.bin
```

## Protocol V2 & Mesh Engine

All multi-byte header fields are encoded in big-endian (network byte order). Packet layout:
`ForwardHead (28B) + ForwardNode[ttl] (8B*ttl) + ForwardFile + data`

- `length` (`uint64_t`): 64-bit total packet size (supports >4 GB large files)
- `magic` (`uint32_t`): `0xFF44AADD`, `version` (`uint8_t`): `2`
- `cmd` (`uint8_t`): `ForwardPull (0)`, `ForwardPush (1)`, `ForwardMeshSync (2)`
- `ttl` (`uint8_t`): remaining hops in packet header (max `32`)
- `flags` (`uint8_t`):
  - bit `0` (`kFlagStoreOnHop = 0x01`): single-pass tee caching on intermediate hops
  - bit `1` (`kFlagStrictRoute = 0x02`): disable Mesh route completion and failover
  - bits `2..7`: cumulative physical `hops_traversed` counter enforcing a hard 32-hop ceiling
- `id` (`uint64_t`): unique 64-bit request ID for cross-node log tracing
- `crc32` (`uint32_t`): IEEE 802.3 CRC32 verified in-flight and at endpoints
- Route persistence: each `fwdd` persists learned neighbors and multi-hop routes to `<dir>/mesh_routes.dat` and reconciles path vectors periodically via `ForwardMeshSync`.
