# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test Commands

```bash
# Standard build (outputs to bin/fwdd, bin/fwd)
make

# Run C++ unit tests (MeshRouter, path-vector sync codec, hop budget, persistence)
make unit-test

# Run all tests (unit + Protocol V2 basic suite + cross-subnet Mesh suite)
make test

# Run AddressSanitizer tests (foreground -F mode; works on Linux & macOS)
make asan-test

# Run memory leak tests (macOS leaks(1))
make leaks-check

# Clean all build artifacts
make clean
```

## Protocol Stack (V2 + Mesh)

Packet layout (big-endian / network byte order on the wire):
`ForwardHead (28B) + ForwardNode[n] + ForwardFile + data`

Key constants in `src/protocol.h`:
- Magic: `0xFF44AADD`, Version: `2`
- Commands: `ForwardPull (0)`, `ForwardPush (1)`, `ForwardMeshSync (2)`
- Flags (`ForwardRequest::flags`):
  - `kFlagStoreOnHop (0x01)`: tee-cache payload on intermediate hops
  - `kFlagStrictRoute (0x02)`: strict source route (`fwd -R`), disables auto-completion & failover
  - `flags >> 2` (`GetHopsTraversed` / `MakePacketFlags`): 6-bit cumulative physical hop counter enforcing `<= 32` total hops
- Default port: `40000`, Max TTL: `32`, Buffer size: `65536`, Heartbeat: `3000ms`

## Architecture

- **`src/mesh.h` / `src/mesh.cc` (`MeshRouter`)**:
  - Passively learns direct neighbors and downstream hop chains from successful Push/Pull transfers.
  - Exchanges path-vector routing snapshots via `ForwardMeshSync` to stitch segmented routes across subnets, measure hop RTT, degrade failed downstream links (`healthy = false`), and prevent loops (`IsSelf` across dynamically discovered NIC IPs + duplicate-hop checks).
  - Persists learned topology atomically to `<dir>/mesh_routes.dat` and reloads on startup.
- **`src/fwdd.cc`**:
  - Supports daemon mode (default) and foreground mode (`-F`), PID file (`-P`), forced hop caching (`-S`), configurable heartbeat interval (`-I ms`), and elastic worker pool scaling (up to 64 threads when base workers are busy).
  - `EstablishNextHop()` expands missing intermediate hops, triggers on-demand neighbor sync on unknown targets, and reroutes around unreachable intermediate hops unless `kFlagStrictRoute` is set.
- **`src/fwd.cc`**:
  - Supports Push (`-f`), Pull (`-g`), intermediate tee-caching (`-s`), strict source routing (`-R`), and live node routing table inspection (`-l`).
- **`src/util.cc` / `src/logger.cc`**:
  - Non-blocking `ConnectWithTimeout()` (supports `FWDD_DENY_PEERS` test hook for simulating cross-subnet isolation on localhost), `RelayStream()` single-pass streaming/tee + CRC32, big-endian wire serialization, and mutex-serialized logging.
