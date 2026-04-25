# ForwardMesh

File forwarding system that routes data through a chain of nodes based on an IP list.

## Build

```
make
```

## Run

**Server:**
```
./bin/fwdd [-p port] [-d dir] [-h]
  -p port  : specify port (default 40000)
  -d dir   : specify working directory
  -h       : show this help
```

**Client:**
```
./bin/fwd -a ip:port,... [options]
  -a addr : comma-separated list of ip:port forwarding chain
  -f file : file to send (push mode)
  -g file : remote file to get (pull mode)
  -h      : show this help
```

## Examples

**Push (send file to server chain):**
```bash
# Single node: client -> Node1
./bin/fwd -a 127.0.0.1:40000 -f test.txt

# Multi-node: client -> Node1 -> Node2 -> Node3
./bin/fwd -a 127.0.0.1:40000,127.0.0.1:40001,127.0.0.1:40002 -f test.txt
```

**Pull (get file from server chain):**
```bash
# Single node: client -> Node1 (get file from Node1)
./bin/fwd -a 127.0.0.1:40000 -g /path/to/remote.txt -f local.txt

# Multi-node: client -> Node1 -> Node2 -> Node3 (get file from Node3)
./bin/fwd -a 127.0.0.1:40000,127.0.0.1:40001,127.0.0.1:40002 -g /path/to/file.txt -f local.txt
```

## Protocol

Packet format: `ForwardHead + ForwardNode[n] + ForwardFile + data`

- Initial: `ForwardHead + ForwardNode*n + ForwardFile + data`
- After hop: `ForwardHead + ForwardNode*(n-1) + ForwardFile + data`
- Final: `ForwardHead + ForwardFile + data`

### Commands

- `ForwardPush` (cmd=1): Send file through chain to final destination
- `ForwardPull` (cmd=0): Request file from final destination and pull back through chain

### Response

Server responds on the same socket (no reverse connection needed).

## Architecture

- `fwdd`: Server daemon (forwards or stores files)
- `fwd`: Client (push or pull files through chain)