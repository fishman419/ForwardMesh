# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ForwardMesh is a file forwarding system that routes data through a chain of nodes based on an IP list. It consists of two binaries:
- `fwdd` - server daemon that receives and forwards files
- `fwd` - client that sends files through the forwarding chain

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

## Code Architecture

### Protocol Stack
The protocol uses a packet format: `ForwardHead + ForwardNode[n] + ForwardFile + data`

When forwarding through a node, the TTL decrements and one ForwardNode is consumed:
- Initial: `ForwardHead + ForwardNode*n + ForwardFile + data`
- After hop: `ForwardHead + ForwardNode*(n-1) + ForwardFile + data`
- Final: `ForwardHead + ForwardFile + data`

Key constants in `src/protocol.h`:
- Magic: `0xFF44AADD`, Version: `1`
- Default port: `40000`
- Max TTL: `32`

### Core Components

**src/protocol.h** - Protocol structures (ForwardRequest, ForwardResponse, ForwardNode, ForwardFile). All structures are packed with `__attribute__((packed))`.

**src/fwdd.cc** - Server implementation with daemonize logic (double-fork), signal handling (SIGCHLD/SIGHUP ignored), and main event loop via `forward_loop()`. Handles two modes:
- `forward_next()` - Forward to next node in chain
- `store_local()` - Write file to disk when TTL reaches 0

**src/fwd.cc** - Client implementation. Parses comma-separated address list and sends file through the forwarding chain.

**src/util.cc** - Sync I/O utilities: `send_sync()`, `recv_sync()`, `forward_sync()` (relay data between fds).

**src/logger.h/cc** - Logging with levels (DEBUG/INFO/WARNING/ERROR), writes to `forward_server.log` by default.

### File Structure
```
src/
  fwdd.cc          # Server (daemon)
  fwd.cc           # Client
  fwdd_optimized.cc # Alternative optimized server (referenced in Makefile.optimized)
  util.cc          # I/O utilities
  protocol.h       # Protocol structures
  logger.h/cc      # Logging module
```

## Running

**Server:**
```bash
./fwdd [-d /path/to/store] [-p port]
```

**Client:**
```bash
./fwd -a ip:port,ip:port,... -f /path/to/file
```