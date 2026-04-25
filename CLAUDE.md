# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Standard build (outputs to bin/fwdd, bin/fwd)
make

# Optimized build with aggressive compiler flags
make -f Makefile.optimized

# Debug build
make -f Makefile.optimized debug

# Clean build artifacts
make clean
```

## Testing

```bash
# Run all tests
./tests/test_basic.sh

# Run memory leak tests (macOS)
make leaks-check

# Run ASAN tests (Linux only - macOS has ASAN + fork + threads issues)
make asan-test
```

## Protocol Stack

The protocol uses a packet format: `ForwardHead + ForwardNode[n] + ForwardFile + data`

When forwarding through a node, the TTL decrements and one ForwardNode is consumed:
- Initial: `ForwardHead + ForwardNode*n + ForwardFile + data`
- After hop: `ForwardHead + ForwardNode*(n-1) + ForwardFile + data`
- Final: `ForwardHead + ForwardFile + data`

Key constants in `src/protocol.h`:
- Magic: `0xFF44AADD`, Version: `1`
- Default port: `40000`
- Max TTL: `32`
- Buffer size: `16384`

Commands: `ForwardPull` (cmd=0) requests file from final destination, `ForwardPush` (cmd=1) sends file through chain.

## Architecture

**fwdd** (server daemon):
- Double-fork daemonize with SIGCHLD/SIGHUP ignored
- Thread pool model: `ForwardLoop(port, num_threads)` creates worker threads that process requests asynchronously
- Main thread handles `accept()` and dispatches connections to thread pool via mutex-protected queue
- Worker threads (`WorkerThread()`) pull tasks from queue and process concurrently
- `ForwardNext()` - store-and-forward to next node (uses temp file for buffering)
- `StoreLocal()` - write file to disk when TTL reaches 0
- `PullForward()` / `PullFileToClient()` - pull mode handlers

**fwd** (client):
- `SendFile()` - push mode: sends file through forwarding chain
- `PullFile()` - pull mode: request file from final node and receive it back
- `ResolveAddress()` - parses comma-separated ip:port chain

**util.cc** - Sync I/O: `SendSync()`, `RecvSync()`, `ForwardSync()` (relay data between fds), `MakeResponse()`

## Usage Examples

**Server:**
```bash
./bin/fwdd [-p port] [-d dir] [-t threads]
```

**Client push (send file through chain):**
```bash
# Single node
./bin/fwd -a 127.0.0.1:40000 -f test.txt

# Multi-node chain
./bin/fwd -a 127.0.0.1:40000,127.0.0.1:40001,127.0.0.1:40002 -f test.txt
```

**Client pull (get file from final node):**
```bash
# Single node
./bin/fwd -a 127.0.0.1:40000 -g /path/to/remote.txt -f local.txt

# Multi-node chain
./bin/fwd -a 127.0.0.1:40000,127.0.0.1:40001,127.0.0.1:40002 -g /path/file.txt -f local.txt
```
