# epoll-coroutine 性能测试报告

**测试日期：** 
**被测代码：** `main` 分支 `9d93ed2`
**测试工具：** `ss` / `nstat` / `perf`
**测试规模：** 1k / 5k / 10k 并发长连接
**协议：** 裸 TCP 行协议（`apple\n` → dictionary response）

---

## 1. 测试结论

本次测试主要验证三个方面：

1. 1k、5k、10k 长连接下的连接状态、内存和 fd 规模；
2. 请求处理路径的 CPU 成本及系统调用开销；
3. `epoll_ctl` 延迟批量合并机制以及 `max_events=10` 对事件循环的影响。

### 核心结果

| 指标               |           1k |               5k |          10k |
| ---------------- | -----------: | ---------------: | -----------: |
| 并发连接             |        1,000 |            5,000 |       10,000 |
| 服务端线程数           |            1 |                1 |            1 |
| 服务端 fd           |        1,006 |            5,006 |       10,006 |
| RSS              |       8.6 MB |            25 MB |      47.5 MB |
| 每连接 RSS          |       8.6 KB |           5.0 KB |   **4.7 KB** |
| 请求总数             |       50,000 |           50,000 |       50,000 |
| 正确率              |         100% |             100% |     **100%** |
| 吞吐               | 20,408 req/s | **28,425 req/s** | 17,901 req/s |
| 每请求 CPU          |      12.6 µs |          25.3 µs |  **53.4 µs** |
| IPC              |         0.66 |             0.46 |     **0.28** |
| cache-miss 率     |        3.22% |           13.62% |   **29.96%** |
| `epoll_ctl` / 请求 |            0 |                0 |            0 |
| `recvfrom` / 请求  |            2 |                2 |            2 |
| `sendto` / 请求    |            1 |                1 |            1 |

主要结论：

* **10,000 长连接可以稳定维持并正确处理请求**：5 万次请求全部成功，0 错误。
* 服务端保持**单进程、单线程**，10k 连接时 RSS 约 **47.5 MB**，约 **4.7 KB/连接**。
* 稳态请求路径中 `epoll_ctl` 为 **0 次/请求**，延迟批量合并机制确实避免了 `epoll_ctl` 系统调用。
* `recvfrom` 为 **2 次/请求**，符合当前 `BlockSyscall` 的设计。
* `max_events=10` 在 5k、10k 规模下已经成为明确的事件处理上限。
* 随连接规模增加，每请求 CPU 从 **12.6 µs → 53.4 µs**，同时 cache-miss 从 **3.22% → 29.96%**，说明当前事件循环存在明显的缓存局部性问题。
* 另外发现一个独立问题：**连接建立吞吐在约 5k～6k 连接后出现严重且非单调的下降，目前尚未定位根因。**

---

# 2. 测试环境

## 2.1 硬件与软件

| 项目     | 配置                                      |
| ------ | --------------------------------------- |
| 平台     | WSL2                                    |
| Kernel | Linux 6.18.33.2-microsoft-standard-WSL2 |
| 架构     | x86_64                                  |
| CPU    | 8 核                                     |
| 内存     | 15.8 GB                                 |
| 编译器    | g++ 15.2.0                              |
| 服务端    | `-std=c++20 -O2 -g`                     |
| ASan   | 未启用                                     |
| 客户端    | Python 3 asyncio                        |
| perf   | 7.0.14                                  |

服务端仍保留逐操作 `std::cout` 追踪，因此本报告中的性能数字应视为当前实现下的**保守下界**。

---

## 2.2 perf 前置配置

本环境默认无法直接读取 perf 所需的内核事件，因此执行：

```bash
sudo apt-get install -y linux-perf

sudo sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid'

sudo mount -o remount,mode=755 /sys/kernel/tracing/
```

确认关键 tracepoint：

```text
syscalls:sys_enter_epoll_pwait
syscalls:sys_enter_epoll_ctl
syscalls:sys_enter_accept
syscalls:sys_enter_recvfrom
syscalls:sys_enter_sendto
```

注意：空闲状态下服务端阻塞在 `epoll_wait`，部分 perf 事件可能显示 `<not counted>`。因此 perf 必须覆盖实际请求处理阶段。

---

## 2.3 文件描述符限制

10k 连接需要约 10,006 个 fd，因此启动服务端时提高 fd 限制：

```bash
nohup bash -c \
  "ulimit -n 1048576; exec ./test_server" \
  > /tmp/server.log 2>&1 &
```

---

# 3. 测试方法

## 3.1 为什么使用自定义 asyncio 客户端

`test_server.cc` 使用的是裸 TCP 行协议，而不是 HTTP：

```text
client: apple\n
server: A fruit that is round and red or green.
```

因此 `wrk` 不适用于该测试：

```text
invalid HTTP method at 1:1
```

本测试使用 Python asyncio 客户端，主要原因：

* 可以同时保持 1k～10k 长连接；
* 可以控制连接建立批次；
* 可以逐连接验证响应；
* 可以控制请求轮次；
* 可以通过 marker file 与 perf 精确同步。

---

## 3.2 单个规模点的测试流程

每个规模点按照以下流程执行：

```text
启动全新服务端
      │
      ▼
建立 N 条长连接
      │
      ▼
记录 ss / /proc 稳态状态
      │
      ▼
perf stat 附着服务端
      │
      ▼
客户端发送 R 轮请求
      │
      ▼
每轮 N 个并发请求
      │
      ▼
总请求数约 50,000
      │
      ▼
perf 窗口结束
      │
      ▼
记录 nstat
      │
      ▼
关闭全部连接
```

连接建立使用：

```text
batch = 10
```

请求轮数：

```text
R = max(5, 50000 / N)
```

因此每个规模点均约处理 50,000 个请求：

|      N |  R |   总请求数 |
| -----: | -: | -----: |
|  1,000 | 50 | 50,000 |
|  5,000 | 10 | 50,000 |
| 10,000 |  5 | 50,000 |

客户端使用：

```text
SO_LINGER = {1, 0}
```

主动关闭时发送 RST，减少不同测试轮次之间 TIME_WAIT 的干扰。

---

# 4. 连接规模测试结果

## 4.1 服务端稳态状态

| 指标               |     1k |     5k |         10k |
| ---------------- | -----: | -----: | ----------: |
| ESTABLISHED      |  1,000 |  5,000 |  **10,000** |
| LISTEN `Recv-Q`  |      0 |      0 |           0 |
| LISTEN `Send-Q`  |      8 |      8 |           8 |
| 服务端 fd           |  1,006 |  5,006 |  **10,006** |
| RSS              | 8.6 MB |  25 MB | **47.5 MB** |
| 线程数              |      1 |      1 |           1 |
| RSS / connection | 8.6 KB | 5.0 KB |  **4.7 KB** |

10k 连接时：

```text
ESTABLISHED = 10000
server fd   = 10006
threads     = 1
RSS         = 47.5 MB
```

说明当前实现能够在单进程单线程模式下维持 10k TCP 长连接。

---

# 5. 请求吞吐与延迟

## 5.1 请求结果

| 指标     |               1k |               5k |              10k |
| ------ | ---------------: | ---------------: | ---------------: |
| 请求轮数   |               50 |               10 |                5 |
| 每轮请求数  |            1,000 |            5,000 |           10,000 |
| 总请求数   |           50,000 |           50,000 |           50,000 |
| 正确     |           50,000 |           50,000 |           50,000 |
| 错误     |                0 |                0 |                0 |
| 总耗时    |           2.45 s |           1.76 s |           2.79 s |
| **吞吐** | **20,408 req/s** | **28,425 req/s** | **17,901 req/s** |

10k 连接下虽然吞吐下降到约 17.9k req/s，但：

```text
50000 / 50000 requests correct
```

没有错误或卡死。

---

## 5.2 延迟

| 指标  |      1k |       5k |      10k |
| --- | ------: | -------: | -------: |
| p50 | 38.9 ms |  82.5 ms | 190.1 ms |
| p90 | 44.4 ms | 156.4 ms | 323.7 ms |
| p99 | 47.8 ms | 182.9 ms | 391.7 ms |
| max | 49.5 ms | 194.0 ms | 408.2 ms |

这里的延迟并不是单请求 service time。

测试模型是：

```text
N 个请求同时发送
        ↓
单线程服务端串行处理
        ↓
后到的请求需要排队
```

因此：

```text
p50 ≈ N / (2 × throughput)
```

例如 N=10k：

```text
10000 / (2 × 17901)
≈ 279 ms
```

与实际 p50=190 ms 同量级。

因此该指标主要用于观察**批量并发请求下的排队行为**，不应作为单请求网络延迟指标。

---

# 6. CPU 成本

## 6.1 perf 结果

每个规模点固定处理 50,000 个请求。

| 指标                  |          1k |          5k |         10k |
| ------------------- | ----------: | ----------: | ----------: |
| cycles              |      2.66 B |      5.14 B |     10.69 B |
| instructions        |      1.77 B |      2.39 B |      3.02 B |
| **IPC**             |    **0.66** |    **0.46** |    **0.28** |
| branches            |       390 M |       545 M |       743 M |
| branch-misses       |      4.44 M |      6.50 M |      9.66 M |
| branch-miss rate    |       1.14% |       1.19% |       1.30% |
| cache-references    |       277 M |       449 M |       822 M |
| cache-misses        |      8.91 M |     61.19 M |    246.15 M |
| **cache-miss rate** |   **3.22%** |  **13.62%** |  **29.96%** |
| task-clock          |      628 ms |    1,264 ms |    2,669 ms |
| **CPU / request**   | **12.6 µs** | **25.3 µs** | **53.4 µs** |

---

## 6.2 结果分析

请求总数固定为 50,000，但随着连接规模增加：

```text
CPU / request

1k      12.6 µs
5k      25.3 µs
10k     53.4 µs
```

10k 相比 1k：

```text
53.4 / 12.6 ≈ 4.2x
```

同时：

```text
cache-miss rate

1k      3.22%
5k     13.62%
10k    29.96%
```

IPC：

```text
1k      0.66
5k      0.46
10k     0.28
```

这说明随着连接规模扩大，CPU 并没有简单地增加“有效计算”，而是越来越多地受到内存访问和缓存未命中的影响。

这与 `processedSockets_` 的实现特征一致：当前结构会保留曾经处理过的 socket，并在事件循环末尾遍历这些 socket。

10k 连接时，该集合可能包含约 10k 个节点：

```text
std::set node
     │
     └──> Socket*
              │
              └──> heap object
```

节点和 `Socket` 对象分散在内存中，因此存在较差的 cache locality。

**需要注意：perf 数据与代码结构高度一致，但仅凭当前数据还不能把 cache-miss 的全部增长严格归因于 `processedSockets_`。应通过修改该结构后的 A/B benchmark 进一步验证。**

---

# 7. epoll / 系统调用分析

## 7.1 epoll 事件

| 指标                      |     1k |        5k |       10k |
| ----------------------- | -----: | --------: | --------: |
| `epoll_pwait`           | 12,962 |     5,010 |     5,005 |
| `epoll_pwait / request` |  0.259 | **0.100** | **0.100** |
| 平均事件 / wakeup           |   3.86 |  **9.99** |  **9.99** |
| `epoll_ctl`             |  **0** |     **0** |     **0** |

5k 和 10k 下：

```text
50000 requests
≈ 5000 epoll_pwait
```

即：

```text
≈ 10 requests / epoll_pwait
```

这与：

```cpp
constexpr static std::size_t max_events = 10;
```

完全一致。

因此可以确认：

> **5k 及以上规模下，`max_events=10` 已经成为事件循环的实际上限。**

---

## 7.2 recv / send

| 系统调用                 |       1k |       5k |      10k |
| -------------------- | -------: | -------: | -------: |
| `recvfrom`           |  100,000 |  100,000 |  100,000 |
| `recvfrom / request` | **2.00** | **2.00** | **2.00** |
| `sendto`             |   50,000 |   50,000 |   50,000 |
| `sendto / request`   | **1.00** | **1.00** | **1.00** |

当前设计中：

```text
recv:
    第一次 → EAGAIN
    第二次 → 获取数据
```

因此：

```text
1 request = 2 recvfrom
```

而 send 通常可以直接完成：

```text
1 request = 1 sendto
```

这说明 `BlockSyscall` 当前确实是通过一次失败的 syscall 进行状态试探。

---

# 8. `epoll_ctl` 延迟合并验证

三个规模点：

```text
epoll_ctl = 0
```

即：

```text
0 epoll_ctl / request
```

这是当前 coroutine + epoll 状态管理策略的一个重要结果。

请求路径中，socket 状态可能经历：

```text
recv operation
    ↓
unwatchRead()
    ↓
下一次 recv operation
    ↓
watchRead()
```

最终状态重新回到原状态。

因此在 `IOContext` 的状态同步阶段：

```cpp
if (socket->io_state_ == io_state)
    continue;
```

直接跳过 `epoll_ctl`。

这意味着稳态请求处理路径没有额外的 `epoll_ctl` 系统调用。

### 结论

README 中关于：

> 延迟批量合并 epoll_ctl，减少高并发场景下系统调用

这一设计在本测试场景中得到了实际验证。

---

# 9. 内核 TCP 状态

一次 10k 连接建立过程：

```text
batch = 10
connection = 10,000
```

完整内核计数器变化：

| Counter                 |               增量 |
| ----------------------- | ---------------: |
| `TcpActiveOpens`        |          +10,002 |
| `TcpPassiveOpens`       |          +10,002 |
| `TcpExtListenOverflows` |              +37 |
| `TcpExtListenDrops`     |              +37 |
| `TcpExtTCPSynRetrans`   |              +37 |
| `TcpExtTCPTimeouts`     |              +37 |
| `TcpExtTCPAbort...`     | 由双方 RST/close 产生 |

其中：

```text
TcpPassiveOpens = +10002
```

说明内核完成了全部连接的 TCP passive open。

同时：

```text
ListenOverflows = +37
ListenDrops     = +37
```

说明建连过程中存在少量 listen queue overflow，但规模相对全部连接数量很小。

需要注意：

> “TCP handshake 成功”与“应用已经 `accept()` 到连接”不是同一件事。因此 `TcpPassiveOpens` 不能单独证明应用层已经处理了所有连接。

---

# 10. 关键发现

## 10.1 `epoll_ctl` 合并机制有效

实测：

```text
1k   → 0 epoll_ctl
5k   → 0 epoll_ctl
10k  → 0 epoll_ctl
```

说明稳态请求路径没有产生额外的 `epoll_ctl` 系统调用。

这是当前实现比较明确的性能优势。

---

## 10.2 `max_events=10` 已成为实际瓶颈

5k / 10k 下：

```text
epoll_pwait / request = 0.100
```

对应：

```text
10 requests / epoll_pwait
```

恰好打满：

```cpp
max_events = 10
```

因此建议至少进行一次 A/B：

```text
max_events = 10
max_events = 128
max_events = 1024
```

比较：

```text
RPS
CPU/request
epoll_pwait/request
p99
```

如果 1024 后：

```text
epoll_pwait/request
```

显著下降，同时 CPU/request 和吞吐改善，就可以证明该参数确实是当前事件循环的限制因素。

---

## 10.3 `processedSockets_` 可能是主要扩展性问题

10k 连接下：

```text
cache-miss rate ≈ 30%
IPC ≈ 0.28
CPU/request ≈ 53.4 µs
```

相比 1k：

```text
cache-miss    3.22% → 29.96%
IPC           0.66  → 0.28
CPU/request   12.6  → 53.4 µs
```

这一趋势与全量扫描 `processedSockets_` 的实现特征一致。

但当前证据更适合表述为：

> `processedSockets_` 是高概率的扩展性热点，需要通过 A/B 修改进一步确认。

而不是直接断言所有 cache-miss 增长都来自该结构。

---

# 11. 异常发现：连接建立吞吐严重退化

## 11.1 现象

同一客户端、同样 `batch=10`：

| 服务端                |        建连耗时 |        速率 |
| ------------------ | ----------: | --------: |
| 极简 Python listener |      1.18 s |   8,467/s |
| 极简 Python listener |      2.22 s |   4,505/s |
| epoll-coroutine    | **58.29 s** | **172/s** |

规模阶梯：

|    N |      1k |      2k |      4k |    6k |      8k |      10k |
| ---: | ------: | ------: | ------: | ----: | ------: | -------: |
| 连接速率 | 8,882/s | 9,023/s | 9,970/s | 892/s | 2,796/s | 70～280/s |

膝点大约出现在：

```text
5k～6k connections
```

但下降并非单调，因此目前不能简单认为存在一个固定的“6k 上限”。

---

## 11.2 已排除因素

| 假设                      | 观测                                            | 判断        |
| ----------------------- | --------------------------------------------- | --------- |
| accept backlog overflow | overflow 较少，且 batch=1000 时 overflow 更多但速度反而更高 | 基本排除      |
| SYN 丢包                  | `TCPSynRetrans` 仅 +37                         | 基本排除      |
| 客户端能力不足                 | 同一客户端对极简 listener 可达到 8k+/s                   | 基本排除      |
| TIME_WAIT               | `SO_LINGER=0`，`timewait=2`                    | 基本排除      |
| 单次 accept 延迟            | 低并发下约 0.3 ms                                  | 基本排除      |
| CPU 饱和                  | 服务端约 28% 单核，客户端约 33%                          | 排除 CPU 饱和 |

---

## 11.3 perf 观察

连接建立期间：

```text
perf window = 45 s
task-clock  = 785 ms
epoll_pwait = 5,154
accept      = 12,406
```

服务端 CPU 占用非常低，说明：

> **当前问题不是 CPU 计算能力不足，而更像是事件循环、coroutine 调度或 socket 状态管理中的等待/调度问题。**

---

## 11.4 当前待验证方向

目前存在以下候选因素：

### A. `processedSockets_`

高连接数下每轮扫描大量 socket，可能造成事件循环处理延迟。

### B. `max_events=10`

监听 fd 和大量连接事件共享同一个 epoll event batch，可能导致 accept 的调度节奏受到影响。

### C. `coroRecv_` / accept 共用 coroutine slot

如果 accept 与 recv 在高并发建立阶段竞争同一个 coroutine handle 槽位，可能产生异常的恢复/覆盖行为。

以上三项目前均属于**待验证假设**，不能仅凭现有数据确定根因。

---

# 12. 下一步 A/B 测试建议

建议不要一次修改多个地方。

### Test A：提高 `max_events`

分别测试：

```text
10
64
128
512
1024
```

记录：

```text
connect/s
req/s
CPU/request
epoll_pwait/request
p99
```

---

### Test B：修改 `processedSockets_`

做两个版本：

```text
baseline
processedSockets_ 每轮扫描
```

以及：

```text
optimized
只维护本轮发生状态变化的 socket
```

固定：

```text
N = 1k / 5k / 10k
requests = 50k
```

比较：

```text
cycles/request
instructions/request
cache-miss/request
IPC
```

---

### Test C：独立测试 accept

暂时不建立 client handler，只测试：

```text
connect
    ↓
accept
    ↓
close
```

分别测试：

```text
1k
5k
10k
20k
```

这样可以把：

```text
accept path
```

与：

```text
recv/send/coroutine path
```

彻底分开。

这是目前定位“10k 建连只有 200/s”的**最重要的一步**。

---

# 13. 测试局限

1. 服务端仍开启逐操作 `std::cout`，会增加 CPU 和 I/O 开销，因此性能数字应视为保守值。
2. 性能测试使用 `-O2 -g`，没有启用 ASan。
3. 全部通信走 `127.0.0.1`，不包含真实网络延迟、丢包和带宽限制。
4. 客户端为 Python asyncio，适合功能和连接规模测试，但不是理想的极限性能 client。
5. perf 统计窗口包含请求阶段及少量空闲等待阶段。
6. `epoll_ctl=0` 只代表当前测试窗口内没有观察到 `epoll_ctl` syscall，并不意味着 teardown 或其他路径永远不会调用 `epoll_ctl`。
7. p50/p99 延迟来自“一批 N 个请求同时发送”的测试模型，主要反映排队行为，而不是单请求网络延迟。
8. 当前 cache-miss 与 `processedSockets_` 之间的因果关系仍需 A/B 实验确认。
9. 连接建立吞吐异常目前尚未定位根因。

---

# 14. 复现

## 14.1 构建

```bash
cd /home/hxl/epoll-coroutine

g++-15 \
  -Wall -Wextra -pedantic \
  -std=c++20 -O2 -g \
  test_server.cc \
  io_context.cc \
  socket.cc \
  socket_accept_operation.cc \
  socket_recv_operation.cc \
  socket_send_operation.cc \
  -o test_server
```

## 14.2 perf 环境

```bash
sudo apt-get install -y linux-perf

sudo sh -c \
  'echo -1 > /proc/sys/kernel/perf_event_paranoid'

sudo mount \
  -o remount,mode=755 \
  /sys/kernel/tracing/
```

## 14.3 启动服务端

```bash
nohup bash -c \
  "ulimit -n 1048576; exec ./test_server" \
  > /tmp/server.log 2>&1 &
```

## 14.4 运行规模测试

```bash
sudo bash /tmp/run_scale.sh 1000 10
sudo bash /tmp/run_scale.sh 5000 10
sudo bash /tmp/run_scale.sh 10000 10
```

结果：

```text
/tmp/client_n<N>.txt
/tmp/ss_idle_n<N>.txt
/tmp/perf_n<N>.txt
/tmp/nstat_before_n<N>.txt
/tmp/nstat_after_n<N>.txt
```

---

# 15. 常用诊断命令

### LISTEN / accept queue

```bash
ss -lnt '( sport = :8080 )'
```

其中：

```text
Recv-Q = 当前 accept queue
Send-Q = listen backlog
```

### TCP 状态

```bash
ss -tan '( sport = :8080 )' |
    awk 'NR>1 {print $1}' |
    sort |
    uniq -c
```

### 内核 TCP 计数器

```bash
nstat -az
```

重点：

```bash
nstat -az |
    grep -iE \
    'ListenOverflow|ListenDrop|TCPSynRetrans|TCPBacklogDrop'
```

### perf

```bash
sudo perf stat \
    -p $(pgrep -x test_server) \
    -e cycles,instructions,cache-misses \
    -- sleep 5
```

### fd / RSS / threads

```bash
PID=$(pgrep -x test_server)

ls /proc/$PID/fd | wc -l

grep -E 'VmRSS|Threads' \
    /proc/$PID/status
```

---

# 16. 最终结论

当前版本在本测试环境下已经证明：

> **单进程单线程模式可以稳定维持 10,000 条长连接，并正确处理 50,000 次请求。**

同时，性能分析发现两个值得优先处理的方向：

### 第一：`max_events=10`

已经在 5k/10k 规模下被完全打满：

```text
10 events / epoll_pwait
```

建议通过 `10 / 128 / 1024` A/B 测试验证其对吞吐和 CPU 的实际影响。

### 第二：`processedSockets_`

随着连接数从 1k 增加到 10k：

```text
CPU/request:    12.6 µs → 53.4 µs
IPC:             0.66   → 0.28
cache-miss:      3.22%  → 29.96%
```

存在明显的规模相关退化。`processedSockets_` 的全量扫描是一个高度可疑的热点，但仍需要修改后的 A/B benchmark 进行因果验证。

此外，**10k 连接建立速度异常下降是当前最值得继续调查的问题**。现有数据已经排除了明显的 CPU、客户端、SYN 重传和 TIME_WAIT 原因，但还不足以确定具体根因。

下一阶段应该优先把测试拆成：

```text
                epoll-coroutine
                       │
            ┌──────────┴──────────┐
            │                     │
        accept benchmark      request benchmark
            │                     │
       connect/s               req/s
       backlog                 CPU/req
       epoll wakeup            cache-miss
                               epoll_ctl
```

先把 **accept 路径**和**请求路径**分离，再对 `max_events` 和 `processedSockets_` 分别做 A/B 测试，才能比较准确地确定当前框架的实际性能上限。
