# ForwardMesh

File forwarding system that routes files through a chain of nodes using source-routed TCP packets.

## Build & Test

```bash
# Build release binaries (bin/fwdd, bin/fwd)
make

# Run integration tests
make test

# Run AddressSanitizer tests
make asan-test

# Run macOS leaks(1) check
make leaks-check
```

## Run

**Server (`fwdd`):**
```text
./bin/fwdd [-p port] [-d dir] [-t threads] [-P pidfile] [-F] [-h]
  -p port     : specify port (default 40000)
  -d dir      : specify working directory (default .)
  -t threads  : number of worker threads (default 4)
  -P pidfile  : write process PID to pidfile
  -F          : run in foreground (do not daemonize)
  -h          : show this help
```

**Client (`fwd`):**
```text
./bin/fwd -a ip:port,... [options]
  -a addr : comma-separated list of ip:port forwarding chain
  -f file : file to send (push) or local output path (pull)
  -g file : remote relative file path to get (pull mode)
  -h      : show this help
```

## Examples

**Push (send file through chain):**
```bash
# Single node: client -> Node1
./bin/fwd -a 127.0.0.1:40000 -f test.txt

# Multi-node: client -> Node1 -> Node2 -> Node3
./bin/fwd -a 127.0.0.1:40000,127.0.0.1:40001,127.0.0.1:40002 -f test.txt
```

**Pull (get file from final node in chain):**
```bash
# Single node: client -> Node1 (relative to Node1's -d working directory)
./bin/fwd -a 127.0.0.1:40000 -g remote.txt -f local.txt

# Multi-node: client -> Node1 -> Node2 -> Node3 (get file from Node3)
./bin/fwd -a 127.0.0.1:40000,127.0.0.1:40001,127.0.0.1:40002 -g remote.txt -f local.txt
```

## Protocol

Packet format: `ForwardHead + ForwardNode[n] + ForwardFile + data`

- Initial: `ForwardHead + ForwardNode*n + ForwardFile + data`
- After hop: `ForwardHead + ForwardNode*(n-1) + ForwardFile + data`
- Final: `ForwardHead + ForwardFile + data`

### Commands & Return Codes

- `ForwardPull` (`cmd=0`): Request file from final destination and pull back through chain
- `ForwardPush` (`cmd=1`): Send file through chain to final destination
- Return codes: `ForwardSuccess (0)`, `ForwardInterrupt (1)`, `ForwardUnreachable (2)`, `ForwardInternalError (3)`, `ForwardInvalidRequest (4)`, `ForwardFileNotFound (5)`

### Architecture

- `fwdd`: Server daemon with configurable worker thread pool, atomic store-and-forward, path confinement to working directory, PID file support, and graceful `SIGINT`/`SIGTERM` shutdown.
- `fwd`: CLI client supporting multi-hop push and pull over a single bidirectional TCP connection per hop.
