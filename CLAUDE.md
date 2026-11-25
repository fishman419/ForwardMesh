# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Standard build (outputs to bin/fwdd, bin/fwd)
make

# Clean build artifacts (including ASAN and leaks build dirs)
make clean
```

## Testing

```bash
# Run integration tests (streaming, tee-caching, concurrency, CRC32 tamper, daemon PID)
make test

# Run AddressSanitizer tests (uses -F foreground mode; works on Linux & macOS)
make asan-test

# Run memory leak tests (macOS leaks(1))
make leaks-check
```

## Protocol Stack (V2)

Packet layout (big-endian / network byte order on the wire):
`ForwardHead (28B) + ForwardNode[n] + ForwardFile + data`

When forwarding through a node, the TTL decrements and one `ForwardNode` is consumed:
- Initial: `ForwardHead + ForwardNode*n + ForwardFile + data`
- After hop: `ForwardHead + ForwardNode*(n-1) + ForwardFile + data`
- Final: `ForwardHead + ForwardFile + data`

Key constants in `src/protocol.h`:
- Magic: `0xFF44AADD`, Version: `2`
- Packet & file size: `uint64_t` (no 4 GB limit)
- Payload integrity: IEEE 802.3 `crc32` verified in-flight and before atomic file rename
- Flags: `kFlagNone (0x00)` = zero-disk cut-through streaming; `kFlagStoreOnHop (0x01)` = single-pass tee caching on intermediate hops
- Default port: `40000`, Max TTL: `32`, Buffer size: `65536`, Socket timeout: `30s`

## Architecture

**fwdd** (`src/fwdd.cc`):
- Supports daemon mode (default) and foreground mode (`-F`), PID file (`-P pidfile`), forced hop caching (`-S`), and graceful `SIGINT`/`SIGTERM` shutdown.
- `CreateListenSocket()` binds before `fork()` so startup port conflicts fail immediately.
- Thread pool model: `ForwardLoop(sockfd, num_threads)` polls the listen socket and dispatches accepted fds to worker threads (`WorkerThread` -> `HandleConnection`).
- `StreamOrTeeForward()` performs zero-disk cut-through streaming by default, or single-pass tee-to-temp-file + atomic `rename()` when `kFlagStoreOnHop` / `-S` is active, rejecting tampered streams on CRC32 mismatch (`ForwardChecksumMismatch = 6`).

**fwd** (`src/fwd.cc`):
- `SendFile()` - computes file CRC32, assigns a traceable 64-bit request ID, and streams payload through the chain.
- `PullFile()` - requests relative file path from final node, streams into `.part.XXXXXX`, verifies CRC32 and final ack, then atomically renames onto target output path.
- `-s` flag sets `kFlagStoreOnHop` on outgoing push/pull requests.

**util.cc / logger.cc**:
- Portable big-endian wire serialization (`SendRequestHeader`, `RecvRequestHeader`, `SendResponseHeader`, `RecvResponseHeader`, `SendNodes`, `RecvNodes`, `SendFileMeta`, `RecvFileMeta`).
- `RelayStream()` unifies socket/file streaming, optional single-pass teeing, and incremental CRC32 calculation.
- `logger.cc` serializes log writes with `std::mutex` and `localtime_r()`.
