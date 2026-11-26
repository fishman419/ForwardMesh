# Mesh Auto-Routing, Heartbeat Sync & Failover Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable zero-config cross-subnet route learning, background path-vector heartbeat synchronization, missing-hop auto-completion, mid-path failover rerouting, route persistence (`mesh_routes.dat`), and optional strict source routing (`fwd -R`) / route table inspection (`fwd -l`).

**Architecture:** Introduce a thread-safe `MeshRouter` module (`src/mesh.h`, `src/mesh.cc`) backed by path-vector route candidates indexed per `(target, next_hop)` and persisted atomically to `<dir>/mesh_routes.dat`. Reuse the existing single TCP port and 28-byte Protocol V2 header by adding `ForwardMeshSync (cmd=2)` for neighbor RTT probing and route table reconciliation, `kFlagStrictRoute (0x02)` for strict mode, and a 6-bit `hops_traversed` counter in `flags[2..7]` to enforce a hard 32-hop wire ceiling.

**Tech Stack:** C++11 (`std::mutex`, `std::condition_variable`, `std::thread`, STL containers), POSIX non-blocking sockets (`fcntl`, `poll`, `getsockname`), GNU Make, Bash + Python3 integration harnesses, AddressSanitizer, macOS `leaks(1)`.

**Spec:** `docs/superpowers/specs/2026-11-26-mesh-auto-routing-design.md`

## Global Constraints

- Single listening port reuse: `ForwardMeshSync` shares `fwdd`'s existing TCP port (`default 40000`).
- Header size invariance: `sizeof(ForwardRequest) == 28` and `sizeof(ForwardResponse) == 28` in big-endian network byte order.
- Hard physical hop ceiling: `kMaxForwardTTL = 32` enforced via `hops_traversed` (`flags` bits `2..7`) + `remaining_tail.size() <= 32`.
- Pure C++11 & POSIX standard libraries only; must compile with zero warnings under `-std=c++11 -Wall -Wextra -pthread` on macOS Clang and Linux GCC.
- Strict mode (`-R` / `kFlagStrictRoute = 0x02`) must never expand missing hops or reroute around failed hops.

## Review Focus

1. **Self-node appearing in `-a` chain (`fwd -a N1,N1` or `N1,N2,N1`)**: `MeshRouter` recognizes `IsSelf(node)` across loopback and dynamically discovered NIC IPs, refusing to store self-routes or dial itself.
2. **Duplicate hops in user-supplied or spliced chains (`[N2, N3, N0] + [N3, N5]` or `N1,N2,N2,N3`)**: `ConcatAndDedupePath` collapses cycles/duplicates into `[N2, N3, N5]` / `[N2, N3]` so a node never forwards to itself.
3. **Multi-NIC self-loop eviction over provisional routes**: When a provisional passive route `[N2, N4]` is followed by `N2`'s `ForwardMeshSync` reporting `[N2, N1_nic2, N4]`, `ApplyNeighborSync` detects `IsSelf(N1_nic2)` and immediately deletes the stale provisional `[N2, N4]` candidate.
4. **Empty route table sync (`route_count == 0` on fresh `fwdd` when running `fwd -l` or initial heartbeat)**: `EncodeMeshSyncPayload` emits a valid 4-byte `0` count with valid CRC32, and `DecodeMeshSyncPayload` / `fwd -l` handles `0` entries cleanly with exit code `0`.
5. **Inbound packet already at hop ceiling (`hops_traversed >= 32`)**: `BuildHopPlans` returns empty plans when `hops_traversed + 1 > kMaxForwardTTL`, terminating any mutually-pointing static route loop (`mesh_routes.dat` misconfiguration) at 32 hops without deadlocking worker threads.

---

### Task 1: Protocol V2 Mesh Extensions, Timeout Dialer, Sync Wire Codec & `MeshRouter` Core (`make unit-test`)

**Files:**
- Modify: `src/protocol.h`
- Modify: `src/util.h`
- Modify: `src/util.cc`
- Create: `src/mesh.h`
- Create: `src/mesh.cc`
- Create: `tests/test_mesh_unit.cc`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `HostToNet64`, `NetToHost64`, `Crc32Update`, `SendSync`, `RecvSync`, `SetSocketTimeout` in `src/util.h`.
- Produces:
  - `src/protocol.h`:
    - `const uint8_t kFlagStrictRoute = 0x02;`
    - `const uint8_t kBaseFlagsMask = 0x03;`
    - `const uint32_t kMaxMeshRoutes = 256;`
    - `const uint32_t kDefaultHeartbeatMs = 3000;`
    - `const int kDefaultConnectTimeoutMs = 2000;`
    - `const int kStrictConnectTimeoutMs = 3000;`
    - `const int kProbeConnectTimeoutMs = 1000;`
    - `enum ForwardCommand { ForwardPull = 0, ForwardPush = 1, ForwardMeshSync = 2 };`
    - `inline uint8_t GetHopsTraversed(uint8_t flags)` -> `(flags >> 2) & 0x3FU`
    - `inline uint8_t MakePacketFlags(uint8_t base_flags, uint8_t hops_traversed)` -> `(base_flags & kBaseFlagsMask) | ((hops_traversed & 0x3FU) << 2)`
  - `src/util.h`:
    - `struct MeshSyncEntry { ForwardNode target; uint16_t rtt_ms; uint8_t alive; std::vector<ForwardNode> path; };`
    - `int ConnectWithTimeout(uint32_t ip, uint32_t port, int timeout_ms, uint16_t *out_rtt_ms);` (returns `>= 0` connected fd with `SetSocketTimeout` applied, `-1` internal socket error, `-2` unreachable/timeout/denied by `FWDD_DENY_PEERS`)
    - `std::string FormatNodeAddr(const ForwardNode &node);`
    - `bool ParseNodeAddr(const std::string &text, ForwardNode *out);`
    - `void EncodeMeshSyncPayload(const std::vector<MeshSyncEntry> &entries, std::vector<uint8_t> *out_buf);`
    - `int DecodeMeshSyncPayload(const uint8_t *buf, size_t len, std::vector<MeshSyncEntry> *out_entries);`
  - `src/mesh.h`:
    - `struct HopPlan { ForwardNode first_hop; std::vector<ForwardNode> remaining_tail; };`
    - `class MeshRouter` with methods:
      - `void Init(uint32_t listen_port, const std::string &persist_path, uint32_t heartbeat_ms);`
      - `void RegisterSelfIp(uint32_t ip_nbo);`
      - `void RegisterSelfFromSocket(int fd);`
      - `bool IsSelf(const ForwardNode &node) const;`
      - `std::vector<ForwardNode> GetDirectNeighbors() const;`
      - `void RecordTransferSuccess(const ForwardNode &first_hop, uint16_t connect_rtt_ms, const std::vector<ForwardNode> &downstream_tail, bool authoritative);`
      - `void RecordConnectFailure(const ForwardNode &hop);`
      - `void RecordDownstreamUnreachable(const ForwardNode &first_hop, const ForwardNode &final_target);`
      - `void ApplyNeighborSync(const ForwardNode &neighbor, uint16_t neighbor_rtt_ms, const std::vector<MeshSyncEntry> &entries);`
      - `std::vector<MeshSyncEntry> ExportSyncSnapshot() const;`
      - `std::vector<HopPlan> BuildHopPlans(const std::vector<ForwardNode> &req_nodes, uint8_t hops_traversed, bool strict_mode, bool *out_should_sync_on_miss) const;`
      - `bool SaveToFile() const;`
      - `bool LoadFromFile();`
      - `static std::vector<ForwardNode> ConcatAndDedupePath(const std::vector<ForwardNode> &prefix, const std::vector<ForwardNode> &suffix);`

- [ ] **Step 1: Write unit tests `tests/test_mesh_unit.cc` and `unit-test` target in `Makefile`**

Create `tests/test_mesh_unit.cc` asserting:
- `UT-01` (`TestRouteRankingOrder`): healthy beats unhealthy; `provisional=false` beats `provisional=true`; 2-hop `[N2, N4]` beats 3-hop `[N3, N5, N4]`; 5ms RTT beats 20ms RTT at equal hops.
- `UT-02` (`TestLoopPreventionAndSelfEviction`): path with duplicate hop rejected; `RegisterSelfIp` for two NICs rejects `[N2, N1_nic2, N4]` and `ApplyNeighborSync` evicts pre-existing provisional `[N2, N4]`; `ConcatAndDedupePath([N2, N3, N0], [N3, N5]) == [N2, N3, N5]`; self-target `[N1_self]` rejected.
- `UT-03` (`TestFlagsBitfieldAndHopBudget`): `MakePacketFlags(kFlagStoreOnHop | kFlagStrictRoute, 17)` round-trips base flags and `GetHopsTraversed == 17`; `BuildHopPlans` rejects expansion when `hops_traversed = 30` and route requires `3` hops (`30 + 3 > 32`), and returns empty when `hops_traversed >= 32`.
- `UT-04` (`TestSyncCodecAndRemoteDegradation`): `EncodeMeshSyncPayload` / `DecodeMeshSyncPayload` round-trips 0 entries and multi-hop entries in NBO, rejecting truncated buffers; `ApplyNeighborSync` with `N3` marked `alive=0` (or omitted) degrades `via N2A` to `healthy=false` so `[N2B, N3]` becomes primary, while single-path `N1->N2A->N3` retains the degraded entry for last-resort fallback (`k == 1`) without `out_should_sync_on_miss` blind direct dial.
- `UT-05` (`TestPersistenceAndCorruptLines`): `SaveToFile` + `LoadFromFile` restores neighbors and routes; corrupt lines (blank, bad IP, port `99999`, >32 hops, self-loop) are skipped cleanly.

Add `unit-test` rule in `Makefile` compiling `src/util.cc src/logger.cc src/mesh.cc tests/test_mesh_unit.cc` into `bin/test_mesh_unit` and running `./bin/test_mesh_unit`.

- [ ] **Step 2: Run `make unit-test` to verify it fails before implementation**

Run: `make unit-test`
Expected: FAIL (`src/mesh.h: No such file or directory`).

- [ ] **Step 3: Implement `protocol.h`, `util.h`/`util.cc`, and `mesh.h`/`mesh.cc`**

Key implementation details not fixed by signatures alone:
- `ConnectWithTimeout`: check `getenv("FWDD_DENY_PEERS")` (comma-separated `ip:port` tokens matched against target `FormatNodeAddr`); if matched, return `-2` (`EHOSTUNREACH`). Otherwise set `O_NONBLOCK`, `connect()`, `poll(POLLOUT, timeout_ms)`, check `getsockopt(SO_ERROR)`, restore blocking mode, call `SetSocketTimeout(fd, kDefaultSocketTimeoutSec)`, compute elapsed `rtt_ms` (`>= 1`).
- `BuildHopPlans`:
  - If `hops_traversed >= kMaxForwardTTL` or `req_nodes.empty()`, return `{}`.
  - `size_t budget = kMaxForwardTTL - (hops_traversed + 1);`
  - Strip any leading `IsSelf` or consecutive duplicate nodes in `req_nodes`; if empty, return `{}`.
  - In `strict_mode`: if `req_nodes.size() - 1 > budget` or `IsSelf(req_nodes[0])`, return `{}`; else return single `HopPlan{req_nodes[0], tail(1..)}`.
  - In auto mode: follow Spec §5.4(2) order (`1a` healthy direct `N_0` -> `1b` healthy multi-hop candidates to `N_0` -> `1c` unknown `N_0` direct attempt + `*out_should_sync_on_miss = true` if other neighbors exist -> `1d` degraded reconnect when `k == 1` -> `2` waypoint bypass to `N_1 .. N_{k-1}` avoiding `N_0`), deduplicating candidate `(first_hop, remaining_tail)` pairs and enforcing `remaining_tail.size() <= budget`.

- [ ] **Step 4: Format and run `make unit-test` & `make` to verify compilation and unit tests pass**

Run: `clang-format -i src/protocol.h src/util.h src/util.cc src/mesh.h src/mesh.cc tests/test_mesh_unit.cc && make unit-test`
Expected: `PASS` on all unit tests (`exit 0`), zero compiler warnings.

- [ ] **Step 5: Commit Task 1**

```bash
git add docs/superpowers/specs/2026-11-26-mesh-auto-routing-design.md docs/superpowers/plans/2026-11-26-mesh-auto-routing.md Makefile src/protocol.h src/util.h src/util.cc src/mesh.h src/mesh.cc tests/test_mesh_unit.cc
git commit -m "feat(mesh): add MeshRouter core, sync wire codec, timeout dialer, and unit tests"
```

---

### Task 2: Integrate Mesh Routing, Heartbeat Sync & CLI Flags into `fwdd` and `fwd`

**Files:**
- Modify: `src/fwdd.cc`
- Modify: `src/fwd.cc`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `MeshRouter`, `HopPlan`, `ConnectWithTimeout`, `EncodeMeshSyncPayload`, `DecodeMeshSyncPayload`, `GetHopsTraversed`, `MakePacketFlags`, `kFlagStrictRoute`, `ForwardMeshSync` from Task 1.
- Produces:
  - `fwdd` CLI option `-I <heartbeat_ms>` (default `3000`, `0` disables background heartbeat thread), `ForwardMeshSync` handler, `SyncNeighborOnce` / `SyncAllNeighborsOnce`, `EstablishNextHop` in `ForwardNext` & `PullForward`, and `<dir>/mesh_routes.dat` load on startup + save on topology change & shutdown.
  - `fwd` CLI options `-R` (sets `kFlagStrictRoute`) and `-l` (queries `ForwardMeshSync` on `forward_address[0]` and prints table to `stdout`).

- [ ] **Step 1: Update `Makefile` object lists to link `mesh.o` into `fwdd` (`build`, `build_asan`, `build_leaks`)**

Add `$(BUILD_DIR)/mesh.o`, `$(ASAN_BUILD_DIR)/mesh.o`, and `$(LEAKS_BUILD_DIR)/mesh.o` to `fwdd` link rules.

- [ ] **Step 2: Implement `fwdd` Mesh integration (`src/fwdd.cc`)**

- Global `MeshRouter g_mesh_router;` and `std::condition_variable g_hb_cv;` + `std::mutex g_hb_mutex;`.
- `SyncNeighborOnce(const ForwardNode &peer)`: dials `peer` with `ConnectWithTimeout(..., kProbeConnectTimeoutMs, &rtt_ms)`; on failure calls `g_mesh_router.RecordConnectFailure(peer)`; on success registers `getsockname` self IP, sends `ForwardMeshSync` request, receives `ForwardResponse` + payload, verifies `crc32`, decodes `entries`, calls `g_mesh_router.ApplyNeighborSync(peer, rtt_ms, entries)`.
- `HandleMeshSync(int fd, const ForwardRequest *req)`: exports `g_mesh_router.ExportSyncSnapshot()`, encodes payload, computes `Crc32Update`, sends `ForwardResponse` + payload.
- `EstablishNextHop(...)`: calls `g_mesh_router.BuildHopPlans(...)`, iterates plans trying `ConnectWithTimeout` (timeout `kStrictConnectTimeoutMs` if strict, `kDefaultConnectTimeoutMs` if auto). If all initial plans fail and `should_sync_on_miss` is true, runs `SyncAllNeighborsOnce()` and retries newly discovered plans for `req_nodes[0]`.
- In `ForwardNext` and `PullForward`: compute `data_len` from inbound `req->length` and `req->ttl` first; after `EstablishNextHop` chooses `(first_hop, remaining_tail)`, set `next_req.ttl = remaining_tail.size()`, `next_req.flags = MakePacketFlags(req->flags, hops_traversed + 1)`, and recompute `next_req.length`. On `ForwardSuccess`, call `RecordTransferSuccess(first_hop, connect_rtt_ms, remaining_tail, (req->flags & kFlagStrictRoute) != 0)`; on downstream `ForwardUnreachable`, call `RecordDownstreamUnreachable(first_hop, req_nodes.back())`.
- In `HandleConnection`: register `RegisterSelfFromSocket(fd)`; reject if `GetHopsTraversed(req.flags) > kMaxForwardTTL`; dispatch `req.cmd == ForwardMeshSync` (allowed with `req.length == sizeof(ForwardRequest)` before `RecvFileMeta`!).
- In `main` / `ForwardLoop`: parse `-I`, call `g_mesh_router.Init(port, dir + "/mesh_routes.dat", heartbeat_ms)` and `g_mesh_router.LoadFromFile()`, start `MeshHeartbeatThread` when `heartbeat_ms > 0` (performs immediate initial probe pass, then waits on `g_hb_cv.wait_for`), notify `g_hb_cv` on shutdown, join heartbeat thread, and call `g_mesh_router.SaveToFile()`.

- [ ] **Step 3: Implement `fwd` `-R` and `-l` options (`src/fwd.cc`)**

- `getopt(argc, argv, "a:f:g:sRlh")`:
  - `-R`: `flags |= kFlagStrictRoute;`
  - `-l`: `list_mode = true;`
- `QueryMeshRoutes(int fd)`: sends `ForwardRequest` (`cmd = ForwardMeshSync`, `length = sizeof(ForwardRequest)`), receives `ForwardResponse` + payload, verifies `crc32`, decodes `MeshSyncEntry` vector, prints formatted header `TARGET | STATUS | HOPS | RTT_MS | PATH` and rows (or `(no routes learned)` when empty), returns `0`.

- [ ] **Step 4: Format, build all targets, and run existing `test_basic.sh` & `unit-test` for regression check**

Run: `clang-format -i src/fwdd.cc src/fwd.cc && make clean && make && make unit-test && ./tests/test_basic.sh`
Expected: Zero warnings; all unit tests and all 9 existing V2 integration tests `PASS`.

- [ ] **Step 5: Commit Task 2**

```bash
git add Makefile src/fwdd.cc src/fwd.cc
git commit -m "feat(mesh): integrate auto-routing, heartbeat sync, strict flag, and route query"
```

---

### Task 3: Cross-Subnet Mesh Integration Suite, Fault/Concurrency Stress, ASAN/Leaks & Docs

**Files:**
- Create: `tests/test_mesh.sh`
- Modify: `tests/test_asan.sh`
- Modify: `tests/test_leaks.sh`
- Modify: `Makefile`
- Modify: `.github/workflows/c-cpp.yml`
- Modify: `README.md`
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: `bin/fwdd`, `bin/fwd`, `FWDD_DENY_PEERS` environment hook, `-I`, `-R`, `-l`, `mesh_routes.dat` from Tasks 1–2.
- Produces: Automated verification covering Spec §7.3 (`IT-01`..`IT-07`) and §7.4 (`FT-01`, `FT-02`, `CT-01`, ASAN, `leaks`, CI).

- [ ] **Step 1: Create `tests/test_mesh.sh` covering `IT-01`..`IT-07`, `FT-01`, `FT-02`, `CT-01`**

Implement self-cleaning (`trap cleanup EXIT`, dynamic `BASE_PORT`) multi-node script testing:
1. `IT-01`: `N1` with `FWDD_DENY_PEERS=127.0.0.1:$P3` fails fast on `fwd -a N1,N3` before learning, and `fwd -a N1 -l` prints `(no routes learned)` with exit `0`.
2. `IT-02`: `fwd -a N1,N2,N3` teaches `N1`; `fwd -a N1 -l` shows `N3`; `fwd -a N1,N3` auto-completes 256KB Push (zero disk on `N2`), `-s` Push (cached on `N2`), and Pull (`cmp -s`).
3. `IT-03`: 4-node 3-subnet chain `N1--N2--N3--N4` (`-I 150`ms, non-adjacent pairs blocked via `FWDD_DENY_PEERS`); separate 1-hop pushes `N1,N2`, `N2,N3`, `N3,N4` converge via heartbeat so `fwd -a N1,N4` auto-completes both missing middle hops.
4. `IT-04`: `-I 0` on-demand sync (`fwd -a N1,N2`, then `fwd -a N2,N3`, then immediate `fwd -a N1,N3` succeeds without sleeping).
5. `IT-05`: Diamond `N1 -> {N2A, N2B} -> N3` + remote sub-hop `N1 -> N2A -> N2A_sub -> N3`:
   - Kill `N2A`: explicit `fwd -a N1,N2A,N3` (Push & Pull) and abbreviated `fwd -a N1,N3` both reroute through `N2B`.
   - Remote link break: keep `N2A` up, kill `N2A_sub`, wait 2 heartbeats -> `fwd -a N1,N3` routes cleanly through `N2B`.
   - Restart `N2A` (direct to `N3`), stop `N2B`, wait 2 heartbeats -> traffic recovers via `N2A`.
6. `IT-06`: Strict `-R` mode: `fwd -R -a N1,N3` (missing hop) fails; `fwd -R -a N1,N2_dead,N3` fails without rerouting; `fwd -R -a N1,N2_alive,N3` succeeds.
7. `IT-07`: Stop `N1` (`SIGTERM`), assert `N1/mesh_routes.dat` exists, restart `N1`, run `fwd -a N1,N3` immediately -> succeeds from persisted routes.
8. `FT-01`: Mutually pointing `mesh_routes.dat` (`N1 <-> N2` for unreachable `N9`) terminates at 32-hop ceiling (`exit != 0` in `< 2s`), followed by a valid `fwd -a N1,N2` succeeding.
9. `FT-02`: Raw Python socket sends truncated `ForwardMeshSync` and bad-CRC32 sync request/payload; `fwdd` rejects cleanly and stays healthy.
10. `CT-01`: `-I 50`ms heartbeat + 24 concurrent `fwd` processes (abbreviated Push, Pull, `-s`, and `-l` queries) all exit `0` with exact `cmp -s`.

- [ ] **Step 2: Expand `tests/test_asan.sh`, `tests/test_leaks.sh`, `Makefile`, CI, and docs**

- Add Mesh auto-completion, heartbeat sync (`-I 150`), and failover reroute cases into `tests/test_asan.sh` and `tests/test_leaks.sh`.
- Update `Makefile` `test` target to run `./bin/test_mesh_unit`, `./tests/test_basic.sh`, and `./tests/test_mesh.sh`.
- Update `.github/workflows/c-cpp.yml` to run `make unit-test`, `./tests/test_basic.sh`, `./tests/test_mesh.sh`, and `make asan-test`.
- Document `-R`, `-l`, `-I`, `ForwardMeshSync`, and auto-routing / failover / `mesh_routes.dat` in `README.md` and `CLAUDE.md`.

- [ ] **Step 3: Build all targets and execute full verification suite**

Run:
- `chmod +x tests/test_mesh.sh tests/test_asan.sh tests/test_leaks.sh && make clean && make && make unit-test && make asan && make bin_leaks/fwdd bin_leaks/fwd`
- `./tests/test_basic.sh`
- `./tests/test_mesh.sh`
- `./tests/test_asan.sh`
- `./tests/test_leaks.sh`
Expected: Every test suite exits `0` with `0` memory errors and `0` leaks.

- [ ] **Step 4: Commit Task 3**

```bash
git add Makefile .github/workflows/c-cpp.yml README.md CLAUDE.md tests/test_mesh.sh tests/test_asan.sh tests/test_leaks.sh
git commit -m "test(mesh): add cross-subnet mesh test suite, ASAN/leaks coverage, and docs"
```
