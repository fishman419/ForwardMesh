# ForwardMesh 自学习路由、心跳同步与故障绕行设计文档

- **日期**：2026-11-26
- **状态**：待确认

## 1. 背景与目标（Intent & Why）

`ForwardMesh` 旨在解决**跨网段多跳文件转发**问题（链路上每一跳均可直接向下一跳发起 TCP 连接，但跨网段节点之间通常无法越级直连）。

在现有 Protocol V2 实现中，`fwdd` 节点完全无状态，每次传输都要求客户端在 `fwd -a ip1:port1,ip2:port2,...` 中完整手写所有中间跳板节点，存在两个核心痛点：
1. 客户端使用者必须清楚整条跨网段跳板拓扑，漏写中间节点即连接失败；
2. 链路上任一中间跳板宕机时，即使网络中存在其他已跑通的备用跳板路径，节点也无法自主绕行。

本次设计的目标是在**保持 `fwdd` 零额外拓扑配置、保持 `fwd -a` 原有使用习惯不变**的前提下，让 `fwdd` 在转发流量的过程中自动学习邻居与多跳路径，通过后台心跳在已学邻居间同步可达路由表并探测健康度，从而支持**缺省中间节点自动补齐**与**中间节点故障就地绕行**，同时为 `fwd` 提供参数开关以支持严格按输入链路转发。

---

## 2. 目标、非目标与底线约束（Goals / Non-Goals / Anti-Goals）

### Goals
1. **流量自学习**：`fwdd` 在成功完成 Push 或 Pull 转发后，自动将本次直连的下一跳记为直连邻居，并将包头中后续可达节点链记入本地路由表。
2. **邻居心跳与路径向量同步**：`fwdd` 后台线程定期向已学直连邻居发送心跳并拉取对方的可达路由表（含完整路径向量以防环），使得分段独立传输（如先传 `N1->N2`、再传 `N2->N3`）也能自动拼接收敛出多跳路由 `N1->N2->N3`，并及时感知节点宕机与恢复。
3. **缺失节点自动补齐**：默认模式下，当客户端传入省略了中间节点的地址链（如 `fwd -a N1,N3`）时，`N1` 自动查表补齐中间节点（`N1 -> N2 -> N3`）完成 Push 或 Pull。
4. **中间节点故障就地绕行**：默认模式下，当客户端传入完整链路（如 `fwd -a N1,N2A,N3`）但 `N2A` 不可达时，若 `N1` 已知另一条通往后续节点 `N3` 的健康路径（如 `N2B -> N3`），`N1` 自动改走备选路径完成投递。
5. **客户端严格模式开关**：`fwd` 新增 `-R`（Strict Route）参数；开启时禁止任何路由补齐与故障绕行，严格按 `-a` 输入序列逐跳连接，任一跳不可达立即返回 `ForwardUnreachable`。
6. **路由持久化与可观测性**：`fwdd` 将学到的邻居和路由持久化到工作目录 `<dir>/mesh_routes.dat`，重启后自动加载并通过心跳校验活性；`fwd` 新增 `-l` 参数可查询指定节点的当前路由表。

### Non-Goals（本次不做）
- 不处理反向穿透（NAT / 单向隔离下内网节点反向长连接打通），前提约束是路径上每一跳均可向下一跳主动发起 TCP 连接。
- 不包含断点续传、分块并行传输、TLS/Token 鉴权（留作后续独立阶段）。
- 不引入局域网广播/组播发现或独立中心注册服务。

### Anti-Goals（绝对避免）
- **绝不产生路由死循环**：任何自动补齐或同步的路径中不得出现重复节点，且报文累计物理经过的总跳数在协议层严格受限于 `kMaxForwardTTL (32)`。
- **绝不在已知非直连的跨网段目标上盲目长阻塞直连**：若目标不在直连邻居表中但已在多跳路由表中，直接走多跳路由，避免被跨网段防火墙 DROP 规则拖满 30 秒超时。
- **绝不破坏 CRC32 端到端完整性与严格模式语义**：开启 `-R` 时绝不擅自改道。

---

## 3. 架构约束（Constraints）

1. **单端口复用**：心跳与路由表同步复用 `fwdd` 现有的监听端口，不额外监听新端口，不增加跨网段防火墙开通成本。
2. **协议兼容性**：基于现有 Protocol V2（28 字节大端序头部 `ForwardRequest` / `ForwardResponse`）扩展新 `cmd` 与 `flags` 位，不改变头部字节长度。
3. **无外部依赖**：保持纯 C++11 标准库与 POSIX Socket 实现，兼容 Linux 与 macOS，零警告通过 `-Wall -Wextra`、ASAN 及 `leaks` 检测。

---

## 4. 方案对比与选型（Approaches）

| 方案 | 核心思路 | 优缺点与取舍 | 结论 |
| :--- | :--- | :--- | :--- |
| **方案 1：纯本地流量记忆（被动式）** | 仅从流经本节点的完整包头中缓存后缀路径，无后台心跳与邻居路由交换 | 实现最简单；但无法将 `N1->N2` 与 `N2->N3` 两段独立历史拼接成 `N1->N2->N3`，且无法在业务流量到来前提前感知邻居宕机 | 不选 |
| **方案 2：流量自学习 + 邻居心跳与路径向量（Path-Vector）同步** | 从业务流量学入直连邻居与初始路径；后台心跳线程定期探测直连邻居 RTT/活性，并拉取邻居的完整路径向量表合并到本地路由表；持久化至 `mesh_routes.dat` | 兼顾零配置、分段拓扑自动拼接、毫秒级主动故障剔除与 BGP 式天然无环；复用同一端口与二进制协议，实现复杂度适中 | **已选定** |
| **方案 3：全网洪泛链路状态图 + 显式节点配置** | 要求启动时手工配置 `-i` / `-n` 并全网广播拓扑边 | 违背零配置自学习诉求，增加部署心智负担 | 不选 |

---

## 5. 详细设计（Detailed Design）

### 5.1 CLI 参数设计

- **`fwd` 客户端**（用户已确认 `-R` 严格模式诉求；`-l` 为可观测性补充 — *Builder Call*）：
  ```text
  ./bin/fwd -a ip:port,... [-f file] [-g remote_file] [-s] [-R] [-l] [-h]
    -R : 严格源路由模式（禁止自动补齐缺失节点与故障绕行；默认允许自动补齐/绕行）
    -l : 查询并打印 -a 首个节点的当前 Mesh 路由表
  ```
- **`fwdd` 服务端**（保持零配置可用；新增可选 `-I` 间隔调优参数 — *Builder Call*）：
  ```text
  ./bin/fwdd [-p port] [-d dir] [-t threads] [-P pidfile] [-F] [-S] [-I heartbeat_ms] [-h]
    -I ms : 后台邻居心跳与路由同步间隔毫秒数（默认 3000ms，设为 0 可关闭后台心跳；测试中可调小如 200ms 加速收敛验证）
  ```

### 5.2 协议扩展（`src/protocol.h`）

1. **`ForwardRequest::flags` 位布局**（保持 28 字节头部不变 — *Builder Call*）：
   - Bit `0` (`0x01`)：`kFlagStoreOnHop`（已有：沿途缓存）
   - Bit `1` (`0x02`)：`kFlagStrictRoute`（新增：严格源路由，禁止当前及下游所有跳执行自动补齐或故障绕行）
   - Bits `2..7` (`0xFC`)：`hops_traversed`（新增 6 位累计已走物理跳数计数器，客户端发包时置 `0`，每经过一个 `fwdd` 转发加 `1`；任何节点在入口或路由改写后若 `hops_traversed + new_ttl > kMaxForwardTTL (32)`，立即拒绝并返回 `ForwardInvalidRequest` / `ForwardUnreachable`，从报文层彻底杜绝瞬态路由不一致导致的无限转发环路）。

2. **`ForwardCommand` 新增控制面命令**：
   - `ForwardMeshSync = 2`：心跳探测与可达路由表拉取。
   - **请求包**：标准 28 字节 `ForwardRequest`（`cmd = ForwardMeshSync`, `ttl = 0`, `length = sizeof(ForwardRequest)`, `crc32 = 0`）。
   - **响应包**：`ForwardResponse`（`cmd = ForwardMeshSync`, `retcode = ForwardSuccess`, `length = sizeof(ForwardResponse) + payload_len`, `crc32 = Crc32(payload)`），后跟负载：
     - `uint32_t route_count`（网络字节序，最多返回 `kMaxMeshRoutes = 256` 条路由项）
     - 连续 `route_count` 个条目，每条格式：
       - `uint32_t target_ip`（NBO）
       - `uint32_t target_port`（NBO）
       - `uint16_t rtt_ms`（NBO，从被查询节点到 `target` 的累计估算 RTT 毫秒）
       - `uint8_t path_len`（从被查询节点到 `target` 的路径长度，`1 <= path_len <= kMaxForwardTTL`）
       - `uint8_t alive`（`1` = 当前健康可达，`0` = 已知故障/降级）
       - `ForwardNode path[path_len]`（从被查询节点出发依次经过的节点序列，末尾元素即 `target`；`port` 为 NBO）

### 5.3 Mesh 路由表结构、降级撤回与防环规则（`src/mesh.h` / `src/mesh.cc`）

每个 `fwdd` 进程维护一个线程安全的全局 `MeshRouter` 实例（内部由 `std::mutex` 保护）：

1. **核心数据结构**：
   - **直连邻居表 `neighbors_`**（Key 为 `(ip, port)`）：
     - `bool alive`：当前直连是否健康（`connect` 失败立即置 `false`，探测或转发成功立即恢复 `true`）
     - `uint16_t rtt_ms`：最近一次成功建连/探测耗时（毫秒，最小记 `1ms`）
     - `uint32_t fail_count`：连续失败次数
     - `uint64_t last_ok_ms`：最近成功时间戳
   - **候选路由表 `routes_`**（Key 为目标 `target = (ip, port)`，Value 为按第一跳邻居 `next_hop = path[0]` 唯一索引的 `RouteCandidate` 列表）：
     - `std::vector<ForwardNode> path`：从本节点出发的完整跳序列 `[next_hop, ..., target]`
     - `uint16_t rtt_ms`：累计 RTT
     - `bool healthy`：下游整条路径是否健康（*Builder Call*：解决“`N2A` 自身存活但下游 `N2A -> N3` 断开”的多跳故障切换问题）
     - `bool provisional`：是否为非严格模式下从缩略包头临时学到的推断路径（*Builder Call*：一旦收到 `next_hop` 的权威 `ForwardMeshSync` 快照，立即用真实拓扑路径覆盖或清除）
     - `uint64_t updated_ms`：最近刷新时间戳（超过 `10 * heartbeat_interval` 未刷新的非健康动态多跳路由自动清理；直连邻居保留在 `neighbors_` 中以便恢复重连）。

2. **权威路由同步与远端故障降级规则**（*Builder Call*）：
   - 当本节点向直连邻居 `H` 执行 `ForwardMeshSync` 成功后，以 `H` 返回的快照为准对本地所有 `next_hop == H` 的多跳路由执行完整对账：
     - 若 `H` 汇报某 `target` 为 `alive == 1` 且 `[H] + entry.path` 无环：更新/插入 `RouteCandidate(healthy = true, provisional = false)`；
     - 若 `H` 汇报 `[H] + entry.path` 包含本节点自身 `IsSelf`（存在回环）：**立即删除**本地原有的 `RouteCandidate(target, next_hop = H)`（防止残留幽灵路由引发乒乓环路）；
     - 若本地原有某多跳 `RouteCandidate(target, next_hop = H)` 在 `H` 本次快照中 `alive == 0` 或已消失：将其标记为 `healthy = false`（使其他健康备选路径如 `N2B -> N3` 立即胜出；同时在单路径场景下保留“该目标属于多跳远端而非直连目标”的记忆，避免退化为盲目直连 `target` 卡超时）。
   - 此外，数据面转发给 `H` 后若收到下游返回的 `ForwardUnreachable`，也立即将对应的多跳 `RouteCandidate(..., next_hop = H)` 标记为 `healthy = false`。

3. **防环校验（Loop Prevention）**：
   - `fwdd` 记录自身 `listen_port_`，并在每次 `accept()` 入站或 `connect()` 出站成功时通过 `getsockname()` 自动收集本机各网卡 IP 集合 `self_ips_`（初始包含 `127.0.0.1`），用于判定 `IsSelf(node)`。
   - 任何写入或拼接的路径必须满足：`1 <= path.size() <= kMaxForwardTTL`、不含重复 `(ip, port)`、不含 `IsSelf(node)`。

4. **最优路径排序规则**：
   - 优先选择 `neighbors_[path[0]].alive == true && candidate.healthy == true` 的健康路径，按 **`provisional` 升序（权威路径优先于临时推断路径）→ `path.size()`（跳数）升序 → `rtt_ms` 升序** 排序。

### 5.4 数据面自学习、自动补齐与故障绕行流程

#### （1）被动自学习时机
- 当本节点直连下一跳 `H` **成功完成一次转发**（返回 `ForwardSuccess`）时：
  - 更新 `neighbors_[H]` 为 `alive = true` 及 `rtt_ms`，确保 `[H]` 1 跳直连路由为 `healthy = true`。
  - 对于本次发往 `H` 的下游节点链 `[D_0, ..., D_{m-1}]`：
    - 若请求带 `kFlagStrictRoute`（显式完整源路由），写入 `provisional = false, healthy = true`；
    - 若请求未带 `kFlagStrictRoute`（下游可能还做了隐式展开），仅在本地尚无去往 `D_j`（经由 `H`）的权威路由时写入 `provisional = true, healthy = true`，供首次心跳到来前立即可用，后续由 `ForwardMeshSync` 替换为精确路径。

#### （2）转发选路与建连（`EstablishNextHop`）
当本节点收到待转发节点链 `req_nodes = [N_0, N_1, ..., N_{k-1}]`（`k >= 1`）时，先提取 `hops_traversed` 并检查剩余跳数预算 `budget = kMaxForwardTTL - (hops_traversed + 1)`：

- **分支 A：`req.flags & kFlagStrictRoute`（严格模式 `-R`）**
  - 仅尝试直连 `N_0`（连接超时 `3000ms`），下游剩余链固定为 `[N_1, ..., N_{k-1}]`。
  - 连接失败立即将 `neighbors_[N_0].alive` 置 `false` 并返回 `ForwardUnreachable`。

- **分支 B：默认智能模式（允许自动补齐与绕行）**
  依次生成候选 `(first_hop, remaining_tail)`（要求 `remaining_tail.size() <= budget`）并尝试 `ConnectWithTimeout(first_hop, 2000ms)`：
  1. **尝试到达首跳目标 `N_0`**：
     - **1a（已知健康直连）**：`N_0` 在 `neighbors_` 中且 `alive == true` → 尝试直连 `N_0`，剩余链 `[N_1, ..., N_{k-1}]`。
     - **1b（已知健康多跳补齐）**：查 `routes_[N_0]` 中所有 `healthy == true` 且首跳存活的候选路径 `P = [H, ..., N_0]` → 拼接 `P + [N_1, ..., N_{k-1}]`（去重环段），尝试连接 `H`。
     - **1c（完全未知目标）**：若 `N_0` 既不在 `neighbors_` 也不在 `routes_`：
       1. 先尝试直连 `N_0`（`2000ms` 超时）；
       2. 若直连失败且本节点已有其他直连邻居，**立即执行一次按需 `SyncAllNeighborsOnce()`**，重查 `routes_[N_0]`，若新学到健康路径则按 1b 补齐连接。
     - **1d（降级兜底重连）**：若上述均未连通，且 `N_0` 曾是直连邻居（`alive == false`）或仅有 `healthy == false` 的多跳路径（且无后续可绕行路标 `k == 1`），尝试重连一次该直连邻居或降级路径首跳，支持故障节点刚恢复时的快速自愈。
  2. **中间节点故障就地绕行（当 `k >= 2` 且步骤 1 无法到达中间节点 `N_0` 时）**：
     - 顺序考察后续路标 `N_idx`（`idx = 1 .. k - 1`）：查询通往 `N_idx` 且不经过失败节点 `N_0` 的健康路径 `P = [H, ..., N_idx]`，构造 `P + [N_{idx+1}, ..., N_{k-1}]`（去重环段、检查预算），尝试连接 `H`。
  3. 若所有候选均无法连通，返回 `ForwardUnreachable`。

#### （3）包头重算与下游不可达重试
- 选定 `(first_hop, remaining_tail)` 后，`new_ttl = remaining_tail.size()`，新 `flags` 中写入 `hops_traversed + 1`，并根据入站真实 `data_len`（Pull 模式为 `0`）精确重算 `next_req.length`。
- 若 `first_hop` 建连成功但在**尚未发送文件数据体前**（如 Pull 模式收到 `ForwardUnreachable`）下游返回 `ForwardUnreachable`，将该多跳 `RouteCandidate` 置为 `healthy = false`；对于 Push 模式，由于流式转发已开始，在收到下游 `ForwardUnreachable` 时同样将对应多跳路由置为 `healthy = false`，使下一次请求立即切到备选路径。

### 5.5 路由持久化格式（`mesh_routes.dat`）

保存在 `fwdd` 工作目录 `<dir>/mesh_routes.dat`（写临时文件 + `rename()` 原子更新）：
```text
# ForwardMesh Route Cache v1
NEIGHBOR 127.0.0.1:40002 1
ROUTE 127.0.0.1:40003 2 127.0.0.1:40002,127.0.0.1:40003
```
- `fwdd` 启动时加载 `mesh_routes.dat` 至内存，并在后台心跳线程启动时立即做首轮探测刷新状态。

---

## 6. 留给实现者自行决定的细节（Left to the Builder）

- 非阻塞 `ConnectWithTimeout` 的 `fcntl(O_NONBLOCK)` + `poll()` + `SO_ERROR` 实现细节。
- `fwd -l` 路由表输出的终端对齐排版格式。
- `mesh_routes.dat` 写盘节流策略（拓扑变更时落盘 + 优雅退出时落盘）。
- 单机跨网段隔离测试钩子：支持可选环境变量 `FWDD_DENY_PEERS=ip:port,...`（仅在设置了该环境变量时生效），在建连前模拟跨网段不可直连（返回不可达），从而无需 root / `iptables` 即可在单机回环网卡上精确复现跨子网隔离拓扑。

---

## 7. 详细测试方案（Testing Plan）

测试分为 **C++ 白盒单元测试、跨网段多进程集成测试、故障与环路注入测试、高并发与内存安全检测** 四个层级，全部纳入 `Makefile` 与 GitHub Actions CI 自动执行。

### 7.1 单机模拟跨网段隔离的方法

在单台开发机或 CI 容器上，所有 `fwdd` 均监听在 `127.0.0.1` 的不同端口。若不做限制，`N1` 在未命中路由时尝试直连 `N3` 会直接走本地回环成功，无法真实检验“跨网段必须经跳板中转”。

为此采用双保险机制验证：
1. **环境变量隔离钩子（`FWDD_DENY_PEERS`）**：启动 `N1` 时注入 `FWDD_DENY_PEERS=127.0.0.1:<N3_PORT>`，使 `N1` 对 `N3` 的直接建连立即按跨网段不可达失败处理，只有走 `N1 -> N2 -> N3` 才能连通；
2. **中间节点观测断言**：结合 `-s` 沿途缓存落盘比对、`fwd -l` 路由表输出以及中间节点 `forward_server.log` 的请求 `id` 追踪，双重确认流量确实经过了预期跳板。

---

### 7.2 C++ 白盒单元测试（`tests/test_mesh_unit.cc` → `make unit-test`）

不依赖真实网络端口与 `sleep` 等待，直接针对 `MeshRouter` 与协议编解码函数做确定性断言（目标耗时 < 200ms）：

| 用例编号 | 测试项 | 输入与构造条件 | 预期断言结果 |
| :--- | :--- | :--- | :--- |
| **UT-01** | 最优路径排序优先级 | 为同一目标 `N4` 注入：直连/多跳、权威(`provisional=false`)/临时(`provisional=true`)、2跳/3跳、同跳数不同 RTT(`5ms` vs `20ms`) | 严格按「健康 > 权威 > 跳数少 > 累计 RTT 低」选出最优路径 |
| **UT-02** | 防环与自环幽灵路由清除 | 1) 注入含重复节点的路径 `[N2, N3, N2, N4]`；<br>2) 注册本机多网卡 `self_ips_` 后，先记录临时路径 `[N2, N4]`，再模拟 `N2` 同步汇报真实路径 `[N2, N1_nic2, N4]`；<br>3) 拼接 `[N2, N3, N0] + [N3, N5]` | 1) 含重复节点的路径被丢弃；<br>2) 检测到 `IsSelf(N1_nic2)` 后不仅丢弃新路径，还**立即清除**原有的 `[N2, N4]` 幽灵候选；<br>3) 拼接结果自动去环化简为 `[N2, N3, N5]` |
| **UT-03** | `flags` 位域与 32 跳物理上限 | 测试 `kFlagStoreOnHop`、`kFlagStrictRoute` 与高 6 位 `hops_traversed` 编解码；构造 `hops_traversed = 30` 且需补齐 3 跳的请求 | 各标志位互不串扰；超出 `kMaxForwardTTL (32)` 预算时拒绝选路 |
| **UT-04** | `ForwardMeshSync` 编解码与远端故障降级 | 序列化/反序列化含多条路径的同步负载；模拟 `N2A` 在同步快照中汇报 `N3` 为 `alive=0`（或缺失），同时 `N2B` 汇报 `N3` 为 `alive=1` | 大端序与 CRC32 校验正确；`via N2A` 候选立即置为 `healthy=false`，选路立即切换至 `[N2B, N3]`；若无 `N2B`，保留 `via N2A` 降级记录而不丢失多跳属性 |
| **UT-05** | `mesh_routes.dat` 持久化与脏数据容错 | 写入正常邻居/多跳路由后重载；混入空行、截断字段、非法 IP、端口号 `99999`、超过 32 跳的超长路径行 | 合法条目完整恢复；所有损坏行被安全忽略，不崩溃、不污染合法路由 |

---

### 7.3 跨网段多进程集成测试（`tests/test_mesh.sh` → 纳入 `make test`）

启动真实 `fwdd` 守护进程集群，设置 `-I 200`（200ms 心跳加速收敛），覆盖端到端业务场景：

| 用例编号 | 测试场景 | 拓扑与操作步骤 | 预期断言结果 |
| :--- | :--- | :--- | :--- |
| **IT-01** | 未学习前跨网段隔离基准 | `N1 -- N2 -- N3`（`N1` 禁止直连 `N3`）。在未跑过任何流量前执行 `fwd -a N1,N3 -f a.txt` | 快速失败（退出码非 0，`ForwardUnreachable`），`N3` 无文件 |
| **IT-02** | 首次全链路学习 + 缺省节点自动补齐（Push / Pull / `-s`） | 1) 执行 `fwd -a N1,N2,N3 -f init.txt`；<br>2) 执行 `fwd -a N1 -l`；<br>3) 省略 `N2` 执行 `fwd -a N1,N3 -f push.bin`（256KB）；<br>4) 省略 `N2` 执行 `fwd -s -a N1,N3 -f cache.bin`；<br>5) 省略 `N2` 执行 `fwd -a N1,N3 -g remote.bin -f out.bin` | 1) 首次推送成功；<br>2) `-l` 显示直连 `N2` 与 2 跳路由 `N3 (via N2)`；<br>3) `push.bin` 送达 `N3` 且 `cmp` 一致，`N2` 零落盘；<br>4) `cache.bin` 在补齐出的中间节点 `N2` 与 `N3` 均落盘一致；<br>5) `out.bin` 拉取成功且 `cmp` 一致 |
| **IT-03** | 分段独立流量 + 心跳路径向量拼接（4 节点跨 3 网段） | `N1 -- N2 -- N3 -- N4`（仅相邻节点互通）。分别独立执行单跳：`fwd -a N1,N2`、`fwd -a N2,N3`、`fwd -a N3,N4`；等待心跳收敛后直接执行 `fwd -a N1,N4 -f hop4.bin` | `N1` 通过心跳同步自动拼接出 `[N2, N3, N4]`；省略中间两跳的 `fwd -a N1,N4` 一次成功送达 `N4` |
| **IT-04** | 按需即时同步（消除心跳周期竞态） | 启动参数设 `-I 0`（关闭定时心跳）。先跑 `fwd -a N1,N2`，再跑 `fwd -a N2,N3`，**不等待**立即执行 `fwd -a N1,N3 -f now.txt` | `N1` 直连 `N3` 被拒后自动触发按需 `SyncAllNeighborsOnce()` 从 `N2` 拉回路由，当场补齐并传输成功 |
| **IT-05** | 菱形双路径：中间节点宕机绕行、远端断链降级与恢复自愈 | 构造菱形拓扑 `N1 -> {N2A, N2B} -> N3` 并完成双路径学习：<br>1) `kill` 停掉 `N2A`，客户端**仍传旧的完整链路** `fwd -a N1,N2A,N3 -f f1.bin` 及 Pull；<br>2) 客户端传缩略链路 `fwd -a N1,N3 -f f2.bin`；<br>3) 构造 `N1 -> N2A -> N2A_sub -> N3` 与 `N1 -> N2B -> N3`，仅停掉 `N2A_sub`（`N2A` 仍存活），等待一次心跳后传 `fwd -a N1,N3`；<br>4) 重新拉起故障节点并停掉 `N2B`，等待心跳后传 `fwd -a N1,N3` | 1) `N1` 连接 `N2A` 失败后自动就地改道 `N2B` 送达 `N3`（Push/Pull 均成功）；<br>2) 自动选健康路径 `N2B` 成功；<br>3) 心跳对账将 `via N2A` 降级为 `healthy=false`，流量切至 `N2B` 不黑洞；<br>4) 恢复后的节点被心跳自动探测重新激活，流量无缝切回 |
| **IT-06** | `-R` 严格源路由模式行为验证 | 在 `N1` 已学到 `N1 -> N2B -> N3` 且 `N2A` 已宕机时：<br>1) 执行 `fwd -R -a N1,N3 -f s1.txt`（缺省中间跳）；<br>2) 执行 `fwd -R -a N1,N2A,N3 -f s2.txt`（指定已挂中间跳）；<br>3) 执行 `fwd -R -a N1,N2B,N3 -f s3.txt`（完整健康链） | 1) 严格拒绝补齐，返回非 0；<br>2) 严格拒绝绕行 `N2B`，返回非 0；<br>3) 正常送达 `N3` |
| **IT-07** | `mesh_routes.dat` 持久化与重启免重学 | 学完 `N1 -> N2 -> N3` 后向 `N1` 发送 `SIGTERM` 退出，断言 `mesh_routes.dat` 存在；重启 `N1` 后直接执行 `fwd -a N1,N3 -f restart.txt` | 重启后的 `N1` 自动加载 `mesh_routes.dat`，无需重新跑全链路即可直接补齐送达 `N3` |

---

### 7.4 异常注入、并发竞争与内存安全测试

1. **环路硬上限注入测试（FT-01，包含于 `test_mesh.sh`）**：
   - 在停机状态下向 `N1/mesh_routes.dat` 与 `N2/mesh_routes.dat` 手工写入互指的伪造静态路由（`N1` 认为去 `N9` 走 `N2`，`N2` 认为去 `N9` 走 `N1`），启动两节点并执行 `fwd -a N1,N9 -f loop.txt`。
   - **断言**：请求在累计物理跳数达到 `32` 跳时立即被终止并返回非 0 错误码，耗时 < 2s，工作线程不死锁；紧接着执行正常的 `fwd -a N1,N2 -f ok.txt` 依然 100% 成功。
2. **畸形控制包与篡改注入测试（FT-02，包含于 `test_mesh.sh`）**：
   - 使用 Python 原始 Socket 发送截断的 `ForwardMeshSync` 包及伪造 CRC32 的同步响应，断言 `fwdd` 返回校验错误且服务保持健康。
3. **高频心跳 + 多客户端并发压测（CT-01，包含于 `test_mesh.sh`）**：
   - 设置 `-I 50`（50ms 高频心跳刷新路由表），并发启动 24 个 `fwd` 进程同时执行缺省路径 Push、Pull、`-s` 缓存与 `-l` 查询，断言全部进程退出码为 `0`、文件逐字节比对一致、日志无错乱。
4. **AddressSanitizer 与 macOS `leaks` 检测（更新 `test_asan.sh` / `test_leaks.sh`）**：
   - 在 `test_asan.sh` 与 `test_leaks.sh` 中加入「全链路学习 → 心跳同步 → 缺省补齐 → 故障绕行 → 持久化落盘」完整链路，确保多线程 `MeshRouter`、心跳线程启停与动态路由扩缩容 **0 内存越界、0 数据竞争崩溃、0 内存泄漏**。
5. **CI 自动化门禁（`.github/workflows/c-cpp.yml`）**：
   - 每次 Push / PR 依次串行运行 `make unit-test` → `./tests/test_basic.sh` → `./tests/test_mesh.sh` → `make asan-test`，任一环节失败即阻断构建。
