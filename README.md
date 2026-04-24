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
./bin/fwd -a ip:port,... -f file [-h]
  -a addr : comma-separated list of ip:port forwarding chain
  -f file : file to forward
  -h      : show this help

Example: ./bin/fwd -a 127.0.0.1:40000,127.0.0.1:40001 -f test.txt
```

## Protocol

Packet format: `ForwardHead + ForwardNode[n] + ForwardFile + data`

- Initial: `ForwardHead + ForwardNode*n + ForwardFile + data`
- After hop: `ForwardHead + ForwardNode*(n-1) + ForwardFile + data`
- Final: `ForwardHead + ForwardFile + data`