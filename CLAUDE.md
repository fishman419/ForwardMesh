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
# Run integration tests
make test

# Run AddressSanitizer tests (uses -F foreground mode; works on Linux & macOS)
make asan-test

# Run memory leak tests (macOS leaks(1))
make leaks-check
```

## Protocol Stack

The protocol uses a packet format: `ForwardHead + ForwardNode[n] + ForwardFile + data`

When forwarding through a node, the TTL decrements and one `ForwardNode` is consumed:
- Initial: `ForwardHead + ForwardNode*n + ForwardFile + data`
- After hop: `ForwardHead + ForwardNode*(n-1) + ForwardFile + data`
- Final: `ForwardHead + ForwardFile + data`

Key constants in `src/protocol.h`:
- Magic: `0xFF44AADD`, Version: `1`
- Default port: `40000`
- Max TTL: `32`
- Max filename length: `4096`
- Buffer size: `16384`
- Socket timeout: `30s`

Commands: `ForwardPull` (`cmd=0`) requests file from final destination, `ForwardPush` (`cmd=1`) sends file through chain.

## Architecture

**fwdd** (`src/fwdd.cc`):
- Supports both double-fork daemon mode (default) and foreground mode (`-F`), plus PID file management (`-P pidfile`) and graceful `SIGINT`/`SIGTERM` shutdown.
- `CreateListenSocket()` binds before `fork()` so startup port conflicts fail immediately with non-zero exit status.
- Thread pool model: `ForwardLoop(sockfd, num_threads)` polls the listen socket and dispatches accepted fds to worker threads (`WorkerThread` -> `HandleConnection`).
- Validates protocol header (`magic`, `version`, `cmd`, `ttl`, packet & filename bounds) and confines paths inside `-d dir` (`IsSafeRelativePath` / `ExtractBasename`).
- `StoreAndForward()` / `StoreLocal()` write into temporary files and atomically `rename()` onto target filenames.
- `ForwardNext()` / `PullForward()` / `PullFileToClient()` handle push and pull chains and propagate `ForwardRetcode` end-to-end.

**fwd** (`src/fwd.cc`):
- `SendFile()` - push mode: sends file through forwarding chain and checks `ForwardResponse::retcode`.
- `PullFile()` - pull mode: requests relative file path from final node and writes local output cleanly.
- `ResolveAddress()` - validates IPv4 (`inet_pton == 1`), port (`1..65535`), and hop limit (`<= kMaxForwardTTL`).

**util.cc / logger.cc**:
- `SendSync()`, `RecvSync()`, `ForwardSync()` handle `EINTR`, premature EOF (`read == 0`), and short writes.
- `logger.cc` serializes log writes with `std::mutex` and `localtime_r()`.
