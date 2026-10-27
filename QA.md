# epoll-coroutine 面试问答手册

> 用法：每节先看「一句话答案」，面试时先给这一句，面试官追问再看「展开」。
> 所有数字都标注了出处和口径，**不要背成"单机 10 万 QPS"这类自己圆不回来的话**。
> 代码引用格式为 `文件:行号`，全部对应本仓库当前工作区。

---

## 0. 先校正三个事实（很重要）

面试官题干里有几处和这个仓库对不上，**主动校正比顺着说安全**：

1. **这不是 Echo Server。** 仓库里有两个业务：
   - `main.cc`：HTTP 一次性词典查询（`GET /?word=apple`，`Connection: close`），短连接。
   - `test_server.cc`：裸行协议的**长连接**词典服务（发 `apple\n`，回释义），压测用它。
   面试开场就说清"我做了两个 handler 形态：一次性 HTTP 和长连接行协议"，能自然引出
   短/长连接、拆包、生命周期等考点。

2. **17.9K req/s 不是"单请求 echo 的上限"。** 它是 1 万条长连接、每轮 1 万请求同时打、
   共 5 万请求的**突发轮次**吞吐；同一套代码在短连接 c≥100 时稳定在 **37.7K req/s**。
   两者口径不同，不能直接混着说。

3. **代码里没有"零拷贝"。** 请求先 `recv` 进协程帧里的 `char buffer[4096]`，再构造
   `std::string` 响应，`send` 再拷进内核。全程至少两次用户/内核之间的拷贝。
   正确措辞是"没有引入额外的中间缓冲/序列化层"，不是零拷贝。

另外两个容易被问穿的现状，先心里有数：

- **没有写缓冲、没有定时器、没有多线程、没有 SIGPIPE 处理、没有 EMFILE 处理。**
  这些不是缺点清单，而是"我知道边界在哪"的素材，见附录 A。
- **压测数字是在保留逐操作 `std::cout` 追踪的情况下测的**（`test_server.cc` 构建时没有
  去掉日志）。所以 17.9K 是**下限**，不是这套设计的上限 —— 这一点在性能追问里是加分项。

### 5 分钟口述版

> 单进程单线程的 Reactor：`IOContext` 里一个 `epoll_pwait` 循环，所有 socket 注册成
> `EPOLLET`。业务代码是 C++20 协程，`co_await socket.recv(...)` 看起来像阻塞调用。
> 非阻塞系统调用被封装成 `BlockSyscall` 这个 CRTP awaiter：`await_ready` 恒 false，
> 所以在 `await_suspend` 里先真调一次 syscall；拿到 `EAGAIN` 才把当前协程的
> `coroutine_handle` 存进 `Socket` 的槽位并挂起，返回 `true`；事件循环收到 `EPOLLIN`
> 就把句柄 `resume()`，协程在 `await_resume` 里重试 syscall。
> `epoll_event.data.ptr` 指向长寿命的 `Socket`（不指向临时 awaiter）。
> epoll 关注集的增删只在内存里记脏位，一轮事件分发结束后批量 `EPOLL_CTL_MOD`。
> 每个连接由 `shared_ptr<Socket>` 持有，handler 协程由 `spawn()` 包一层 `oneway_task`
> 自行推进。1 万长连接实测 10000/10000 正确、fd 10006~10007、RSS 47.5MB、单线程。

---

## 1. 项目整体与设计取舍

### Q1. 用 2~3 分钟讲清架构：几个线程？连接怎么创建/销毁？一次请求的完整路径？

**一句话**：单线程、一个 epoll 循环、每连接一个协程；连接在 accept 时创建并交给
`spawn`，在 handler 协程返回、最后一个 `shared_ptr` 释放时销毁。

**完整路径（长连接版）**：

1. `main` 构造 `IOContext`（`epoll_create1`）和监听 `Socket`（bind/listen/非阻塞/注册 epoll）。
2. `io_context.spawn(accept(listen))`：`spawn` 用一个 `oneway_task` 包装（`io_context.cc:71`），
   让 accept 协程立刻开始跑。
3. accept 循环 `co_await listen.accept()`。`SocketAcceptOperation::syscall()` 先在
   非阻塞监听 fd 上试探一次 `accept()`；`EAGAIN` 就把 handle 存到 `socket->coroRecv_`
   （监听 socket 与 recv 共用槽位）并挂起。
4. 新连接进来 → epoll 报告监听 fd `EPOLLIN` → `resumeRecv()` → `await_resume` 重试
   `accept` → 得到 fd → 构造 `shared_ptr<Socket>` → `io_context.spawn(handler(socket))`。
5. handler 里 `co_await socket->recv(buf, 4096)`：同理由试探、EAGAIN、挂起到
   `Socket::coroRecv_`。
6. 客户端发数据 → `EPOLLIN` → `resumeRecv()` → 重试 `recv` 拿到数据。
7. 业务查词典、拼响应、循环 `co_await socket->send(...)` 直到写完。`send` 若 `EAGAIN`
   则挂起到 `coroSend_` 并给 epoll 加 `EPOLLOUT`，可写时再 `resumeSend()`。
8. handler `co_return` → 协程帧销毁 → 帧里的 `shared_ptr<Socket>` 释放 → `~Socket`
   调 `epoll_ctl(DEL)` 并 `close(fd)`。
9. Ctrl-C → signalfd 可读 → 退出循环 → `cleanIO()`：对 `processedSockets_` 里的每个
   socket 调 `cancel()`，`canceled=true` 后 resume 挂起句柄，各操作的 `syscall()` 抛异常，
   协程栈展开；异常路径的 task 被推进 `delayDestructor_`，最后统一清空。

**关键点要主动说**：没有线程池、没有锁、没有 work stealing；"挂起"是回到 epoll 循环，
不是阻塞线程。

### Q2. 为什么 epoll 而不是 select/poll、io_uring、asio？

**一句话**：这个项目的目标是"用最小依赖把 Linux 就绪模型和协程接起来"，epoll 是
就绪式 API 里最直接、和 `coroutine_handle` 映射最自然的。

- **select**：`FD_SETSIZE` 默认 1024，每次调用全量拷贝 fd 集，返回后还要 O(n) 扫位图。
  万级连接直接出局。
- **poll**：没有 fd 上限，但每次 `poll` 仍要传全量数组、内核 O(n) 扫描；连接数大时
  系统调用开销随 n 线性涨。
- **epoll**：`epoll_ctl` 增量注册（红黑树 O(log n)），`epoll_wait` 只返回就绪链表里的
  fd，规模大时接近 O(就绪数)；`data.ptr` 允许挂任意用户指针，正好用来挂 `Socket`。
- **io_uring**：完成式模型，可以批量提交/收割、减少 syscall、支持 fixed buffer，
  理论上更适合高吞吐；但要新内核、编程模型复杂（SQ/CQE、内存序、注册缓冲区），
  而且本项目"挂起协程 → 等 fd 就绪 → 恢复"的映射在完成式模型里要重写成
  "提交 SQE → CQE 到达 → 恢复"，是一次架构级重写。**当前规模（1 万连接、几万 QPS）
  epoll 完全够用，io_uring 是过度设计。**
- **asio**：跨平台、成熟、proactor 抽象，但引入第三方依赖、模板实例化重、调试和
  定制（比如自定义 awaiter/帧分配）都更绕；这个项目的教学目的就是自己实现这层。

**追问"epoll 一定比 poll 快吗"**：连接少且都活跃时差别不大；epoll 的优势在
"连接多、活跃少"和"注册不频繁"。本项目 ET + 事件驱动，正好是它的甜点区。

### Q3. 为什么协程而不是回调/状态机/future-promise？

**一句话**：把"一个请求的时序"写成直线代码，避免手写状态机和上下文对象；代价是
堆上协程帧和调试/取消能力要自己补。

- 回调：每个挂起点都要拆状态，连接对象里塞一堆 `enum State` 和成员变量，控制流
  被切碎；错误处理要穿透每层回调。
- 状态机（如 muduo 的 `TcpConnection` 风格）：本质还是回调，只是把状态收进对象；
  可读性比裸回调好，但每个新业务协议还要再写一套。
- future/promise：`std::future` 在 C++20 没有 `.then`，链式异步要自己搭；而且
  `std::future` 的 `get()` 是阻塞的，不能直接在单线程循环里用。
- 协程（`std::lazy`）：`co_await recv()` 之后代码接着写，挂起/恢复由编译器生成；
  业务层不用知道 epoll。

**代价要主动说**：每个协程帧一次堆分配、调试器对挂起帧的栈回溯不友好、C++20 没有
内置取消（本项目用 `canceled` 标志 + 关停时 resume 展开）、异常路径要自己设计
（`exception_ptr` 存取 + 延迟析构）。

### Q4. 哪些代码是你自己写的，哪些是参考的？

**作答原则**：按 git 历史如实说，**不要含糊**。这个仓库的历史是清晰的：

- `lazy.hh`：**不是自己写的**，是 WG21 `std::lazy` 参考实现（提案 P2506R0 / D2506R0）
  整份粘贴进 `std` 命名空间的 vendored 代码。我做的是：读懂它的对称传输/帧分配/
  异常存取，并在用 g++-15 编译时修了它第 245 行 `_Data_` 的拼写错误（成员名是 `_Data`）。
- `block_syscall.hh`、三个 `*_operation.{hh,cc}`、`socket.{hh,cc}`、`io_context.{hh,cc}`、
  `main.cc`、`test_server.cc`：自己设计并实现。
- `BENCHMARK.md` / `test.md` 里的压测脚本、perf/nstat 采集：自己搭的测试流程，
  文档整理过程中用了 AI 辅助，但数据是自己跑出来的（附复现命令）。
- 设计参考：epoll + 协程的拆法参考了 cppcoro/libunifex 的 awaiter 思路和 muduo 的
  Reactor 分层；但代码不是抄的，`BlockSyscall` 的"在 await_suspend 里发起 syscall"
  和"延迟批量 epoll_ctl"是这个实现自己的选择。

**如果被问"哪些地方你还没完全吃透"**：诚实说 `lazy.hh` 的 type-erased allocator
（`_Promise_allocator<void>`，`lazy.hh:118-170`）只读到"帧后面存一个 dealloc 函数指针"
这一层，没有逐行验证过所有分支；这比装作全会安全。

### Q5. 和 muduo / asio / Seastar 比，差在哪？

**一句话**：差在多线程模型、定时器/缓冲/背压这些工程件，以及后端选择（io_uring）。

| 维度 | 本项目 | muduo | asio | Seastar |
|---|---|---|---|---|
| 线程模型 | 单线程 | one loop per thread + 线程池 | 可配 io_context 多线程 | share-nothing，每核一线程 |
| 调度 | epoll 就绪 → resume 协程 | epoll/poll 回调 | proactor/reactor 抽象 | 自有 reactor + future |
| 定时器 | **无** | TimerQueue（timerfd） | steady_timer | 有 |
| 写缓冲/背压 | 无用户态写队列（请求-响应天然有界） | `Buffer` + 高水位回调 | 有 | 有 |
| 取消 | 自研 `canceled` 标志 | 关连接 | `cancellation_slot` | 有 |
| 后端 | epoll | epoll/poll | epoll/kqueue/IOCP/io_uring | io_uring/自研 |
| 多核扩展 | **无** | 有 | 有 | 有 |

**加分说法**："我没有做多线程和 work stealing，也没有 io_uring 后端；连接对象是
`shared_ptr` 分散分配、没有对象池/SOA。这些在 1 万连接、单机几万 QPS 下不是瓶颈，
但连接数上到几十万、或者要跑满多核时就必须补。"

### Q6. 需求变成 100 万连接 / 多核 / 加业务协议，怎么改？

**100 万连接**：

- fd 上限：`ulimit -n`、`fs.file-max`、`fs.nr_open`、systemd `LimitNOFILE`；
  一个进程 100 万 fd 要多进程/多 loop 分摊。
- 内存：当前 4.7KB/连接 → 100 万就是 4.7GB；**先砍那个 4KB 的协程帧 buffer**
  （改 512B 或按需从池里取），目标 1~2KB/连接；内核 sk_buff/收发缓冲另算，
  用 `tcp_rmem/tcp_wmem` 收紧。
- epoll：epoll 本身能撑百万 fd（红黑树 + 就绪链表），但 `max_events=10` 这种值必须
  提到 1024+；`processedSockets_` 的每轮全量扫描必须改成脏集合。
- accept：`listen(fd, SOMAXCONN)`（当前是 8，见 Q12）、多进程
  `SO_REUSEPORT` 或 `EPOLLEXCLUSIVE` 分摊 accept。
- 短连接场景还要处理客户端端口耗尽 / TIME_WAIT（`tcp_tw_reuse`、连接复用）。

**多核**：

- 方案 A（推荐）：`SO_REUSEPORT`，每线程一个 `IOContext` + 一个监听 socket，
  内核按四元组哈希分流 —— 接近 Seastar 的 share-nothing，无锁。
- 方案 B：muduo 式，一个 accept 线程 round-robin 把新 fd 分给各 loop；跨线程交付
  socket 要用 eventfd 唤醒目标 loop，且 fd 的 epoll 注册必须在目标 loop 线程做。
- 共享业务状态（词典）只读，可无锁；要写就得加锁或分片，或者改成消息传递。
- 定时器/超时按 loop 各自维护，避免全局锁。

**加业务协议**：

- 先抽 `Connection` / `Codec` 层：`onData(Buffer&)` 解析 → 产出完整消息 → 业务
  协程；`send` 走有界发送队列 + 高水位背压。
- 分帧：长度前缀最稳（`uint32 len` + payload），或分隔符（当前长连接就是 `\n`，
  但没有跨 recv 的累积缓冲，见 Q16）。
- handler 不再假设"一次 recv = 一个完整请求"。

---

## 2. epoll 与网络 IO

### Q7. epoll 原理：为什么 epoll_wait 接近 O(1)？和 poll 的本质区别？

**一句话**：epoll 把"注册"和"等待"拆开，fd 注册进红黑树，就绪时由内核回调把
`epitem` 挂到就绪链表；`epoll_wait` 只取走就绪链表，不需要每次传全量 fd。

- `epoll_ctl(ADD)`：fd → `epitem` 插入红黑树，O(log n)；同时给该 fd 的文件层挂
  `ep_poll_callback`。
- 数据到达：内核在 socket 的等待队列上回调 → 把 `epitem` 链入 `rdllist`。
  **就绪事件是"推"进来的，不是 wait 时"扫"出来的。**
- `epoll_wait`：拿 `rdllist` 里的项，拷到用户数组；复杂度 O(就绪数)，与总连接数无关。
- poll：每次调用把整个 `pollfd` 数组拷进内核，内核 O(n) 遍历每个 fd 的
  `poll` 方法；返回后用户还要 O(n) 扫 `revents`。连接多、活跃少时全是无效功。
- 还有一点常被忽略：epoll 的 fd 是**持久注册**的，每次 wait 不重复传集合。

### Q8. LT 与 ET 的区别？你用的哪种？ET 下为什么必须读到 EAGAIN？写事件怎么处理？

**一句话**：用 ET（`io_context.cc:85`：`EPOLLIN | EPOLLET`）。ET 只在"不可读→可读"
的状态跳变时通知一次，所以必须在一次通知里把数据读空；写方向同理。

- **LT**：只要 fd 可读，每次 `epoll_wait` 都返回。忘记读也不会丢事件，但会反复唤醒。
- **ET**：只在状态变化时返回一次。如果一次 `recv` 没读空，剩余数据不会再触发事件，
  连接就"饿死"在缓冲区里。
- 标准写法：`while ((n = recv(...)) > 0)`，直到 `-1 && errno == EAGAIN` 才收手。

**本项目怎么满足 ET**：靠 `BlockSyscall` 的"每次 `co_await` 先真调一次 syscall"。
handler 处理完一个请求后循环回 `recv`，此时 `await_suspend` 里先试探性 `recv`：
有数据就直接拿到（不挂起），没数据才 EAGAIN 挂起。也就是说**每次循环都会主动把
内核缓冲抽干一次**，不依赖"这一轮 epoll 事件没读完"。

但有两个真实边界要说清：

- **`echo_socket`（HTTP）只 recv 一次**，因为它一次性请求后就 `Connection: close`。
  如果客户端在一个包里发了第二个请求，服务端不会处理（本来也是短连接语义）。
- **长连接 handler 没有跨 recv 的累积缓冲**：它假设"一次 recv = 一行完整请求"。
  如果一行被 TCP 拆成两个段，或者一次读到两行，逻辑就错了。这是加协议时必须先修的。

**写事件 ET 怎么处理**：`send` 抛 `EAGAIN` 时注册 `EPOLLOUT` 并挂起；可写时
`resumeSend()`，`await_resume` 重试。发送完成的路径上 `~SocketSendOperation`
会 `unwatchWrite`，于是空闲连接**不会**长期订阅 `EPOLLOUT`（避免 LT 下可写事件
空转，也减少 ET 下的无谓唤醒）。

### Q9. 没用 EPOLLONESHOT，会重复触发吗？

**一句话**：单线程里不会出现"两条线程同时处理同一个 fd"的竞争，所以不需要
`EPOLLONESHOT`。ET 本身不会对同一状态重复通知。

- `EPOLLONESHOT` 解决的是多线程共享监听/连接 fd 时，一个事件被多个线程同时取走、
  并发处理的问题。本项目只有一个循环线程，事件被 resume 一次就消费掉了。
- 但如果将来做多线程（多个 loop 共享 fd 或共享 accept），就需要
  `EPOLLONESHOT` + 处理完后重新 arm，或者 `EPOLLEXCLUSIVE`（只给一个 wait 者）。
- **真实的重复触发风险来自代码本身**：见 Q25 的"句柄槽不置空"。恢复后
  `coroRecv_`/`coroSend_` 仍指向旧句柄，理论上一次错误的 EPOLLIN 可能 resume
  到已经不在 recv 的协程。当前串行 handler 没踩到，但这是应该修的隐患。

### Q10. epoll_wait 返回后的处理流程？EPOLLERR / EPOLLHUP / EPOLLRDHUP 怎么处理？

**本项目实际流程**（`io_context.cc:44-66`）：

1. 先判 `event.data.fd == sigint_fd`（信号 fd 用 `data.fd`，socket 用 `data.ptr`，
   顺序不能反，否则会把一个整数当指针解引用）。
2. `static_cast<Socket*>(event.data.ptr)`。
3. `EPOLLIN` → `resumeRecv()`；`EPOLLOUT` → `resumeSend()`；槽位为空时是空操作。
4. 全部事件处理完，遍历 `processedSockets_`，对新旧关注集不同的做一次 `EPOLL_CTL_MOD`。

**错误事件：当前没有显式处理。** 正确做法是：

- `EPOLLERR`：socket 出错（如 `ECONNRESET`），应直接走关闭流程（`getsockopt(SO_ERROR)`
  看具体错误）。
- `EPOLLHUP`：对端完全关闭，通常伴随 `EPOLLIN`，读会返回 0；可以 `shutdown` + 关闭。
- `EPOLLRDHUP`：需要显式注册才有，表示对端半关闭（收到 FIN），适合实现"读完再写"。

**现状为什么没炸**：Linux 上连接被 RST/FIN 时，通常同时报告 `EPOLLIN|EPOLLERR|EPOLLHUP`，
本项目因为检查了 `EPOLLIN` 而恢复 recv，`recv` 返回 0 或 `-1`，handler 的
`if (n <= 0) co_return` 把它当正常关闭处理了。**这是"碰巧能工作"，不是正确处理**；
如果只报 `EPOLLERR` 而不带 `EPOLLIN`，这个连接就会被静默挂在 epoll 里。

### Q11. accept 策略：非阻塞、批量 accept、EMFILE 怎么办？

- **非阻塞**：`Socket(int fd, ...)` 里 `fcntl(fd_, F_SETFL, O_NONBLOCK)`（`socket.cc:76`），
  监听 fd 也一样。
- **批量 accept**：ET 下必须一次通知把 accept 队列抽干。本项目靠 accept 循环 +
  每次 `co_await` 的试探性 `accept` 实现：只要队列还有连接，`await_suspend` 里的
  syscall 就直接成功，不会挂起，循环继续；直到 EAGAIN 才让出。
- **`TCP_DEFER_ACCEPT` / `SO_REUSEPORT` / `TCP_FASTOPEN`** 都没用；后者需要业务
  和安全策略配合，不是纯优化。
- **EMFILE**：**当前是 bug。** `BlockSyscall::await_suspend` 只在 `EAGAIN/EWOULDBLOCK`
  时挂起（`block_syscall.hh:23`）。`accept` 返回 `-1/EMFILE` 时走"不挂起"分支，
  `await_resume` 返回 -1，`Socket::accept()` 抛 `runtime_error{"accept"}`，
  **accept 循环的 catch 把它吃掉并退出循环 → 服务器从此不再接受新连接**。
  正确做法：
  1. 预留一个 `open("/dev/null")` 的 fd，遇到 EMFILE 就 `close` 它、`accept` 一个
     连接再立刻 `close`，腾出位置；
  2. 或者在这种情况下睡一小会儿（定时器）再重试；
  3. **无论如何不能退出 accept 循环**，还要打指标告警。

### Q12. 万级连接的现实约束：ulimit、maxevents、每连接内存、TIME_WAIT

| 约束 | 本项目实测/设置 | 说明 |
|---|---|---|
| `ulimit -n` | 默认 soft 10240，测试时抬到 1048576 | 1 万连接要 10006~10007 fd，默认值只够"刚好" |
| `listen` backlog | **8**（`socket.cc:28`） | 致命：见下 |
| epoll `max_events` | **10**（`io_context.hh:39`） | 5k/10k 下每轮正好取 10 个，成为绑定约束 |
| 每连接内存 | RSS 47.5MB / 1 万 ≈ 4.7KB | 其中约 4KB 是协程帧里的 `char buffer[4096]` |
| TIME_WAIT | 短连接 + 服务端主动 close | `SO_REUSEADDR` 必须真正生效（原来的 `int opt;` 没初始化，见 BENCHMARK 6.2） |
| accept 队列溢出 | batch=200 时 `ListenOverflows` +4835，卡死 30~62 条 | 根因就是 backlog=8 |

**backlog=8 的机制**（面试可以讲得很细）：三次握手由内核完成，连接进 accept 队列；
队列满后新连接被**丢弃**（`tcp_abort_on_overflow=0` 时不回 RST）。客户端已经收到
SYN-ACK，认为连接 ESTABLISHED；服务端却从未 `accept` 它，于是客户端发的请求永远
读不到，直到超时。压测里"客户端 ESTABLISHED、服务端 `ss` 查无此连接"就是这个。
改成 `SOMAXCONN` 即可（本机 4096）。

### Q13. 粘包/拆包：加协议怎么设计？

**现状**：HTTP 版是"一次 recv 当完整请求"；长连接版是"一次 recv 当一行"。两者都
没有跨 recv 的累积缓冲，属于**协议层未完成**。

**设计**：

- **长度前缀**（首选）：`[uint32 length][payload]`。读端循环：
  `while (buf.size() >= 4) { len = peek(); if (buf.size() < 4+len) break; 取出完整帧; }`。
  优点是无歧义、可预分配、适合二进制。
- **分隔符**（如 `\n`）：简单、可读，但需要处理分隔符跨包、单行超长（要设上限）。
  当前长连接用的就是这种语义，缺的是累积缓冲和"一行可能不完整"的判断。
- **定长**：只在固定请求下可用，不通用。

**和本项目的结合**：把 `char buffer[4096]` 换成每连接一个 `std::vector<char> inbuf`
（或用环形缓冲/`std::string` + erase 头部），`recv` 追加，然后循环 parse。
ET 下仍然要读到 EAGAIN；解析产出的每个完整帧交给业务协程。发送侧同理要有
`outbuf` + 只注册需要的 `EPOLLOUT`。

### Q14. 背压：客户端发得比服务端写得快，写缓冲会 OOM 吗？

**一句话**：本项目**没有用户态写缓冲**，所以不会 OOM；背压是内核发送缓冲 + 协程挂起
自然形成的。

- handler 是"读一个请求 → 生成一个响应 → 循环 send 到写完"，同一连接同一时刻最多
  只有一个响应在途；没有异步任务往队列里堆响应。
- 客户端不读时，内核发送缓冲满 → `send` 返回 `EAGAIN` → `EPOLLOUT` 挂起 → 这个
  连接的业务协程停住，不再读新请求。**背压沿连接传播，不会扩散到整个进程。**
- 代价：没有超时，一个恶意慢客户端可以永久占住连接（见 Q15）。
- 如果以后要支持"服务端主动推送/一个请求多个响应/并发处理"，就必须引入**有界**
  发送队列：高水位时停止读（`unwatchRead` 或不再 recv）、低水位恢复；否则队列
  无界增长就是 OOM。

**主动加分**：可以指出"当前模型是 request-response，天然不需要写缓冲；这不是省了，
而是业务形态还没逼出这个需求"。

### Q15. 半关闭、心跳、空闲超时怎么实现？TCP keepalive 为什么不够？

**现状**：都没有。

- 半关闭：`recv` 返回 0 时 handler `co_return`，`~Socket` 直接 `close`。没有
  `shutdown(SHUT_WR)`，也不支持"读完了还要继续写"的半关闭语义。
- 心跳/空闲超时：**完全没有**。一个客户端连上后不发数据，连接会一直挂在 epoll 里。
- TCP keepalive：默认 `tcp_keepalive_time=7200s`（2 小时），且只在"连接空闲"后
  才开始探测；对应用层的"用户已经走了但 TCP 没断"（比如手机掉网、NAT 超时）
  基本无效。

**实现方案**（能说清就是加分）：

1. **timerfd**：一个（或每 loop 一个）timerfd 注册进 epoll；最小堆/时间轮存每个
   连接的 `lastActive + timeout`；`epoll_wait` 醒来时处理到期连接。
2. **时间轮**：连接多、超时种类多时 O(1) 插入/删除，适合大量定时器。
3. **应用层心跳**：协议里加 ping/pong，超过 N 个周期没收到就 `shutdown` + 关闭；
   同时把"最后活跃时间"在每次 recv/send 时更新。
4. **TCP 层**只能作为兜底：`SO_KEEPALIVE` + `TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT`
   调短，但不要依赖它做业务超时。

---

## 3. C++20 协程（重点）

### Q16. `co_await expr` 展开后发生了什么？

**一句话**：编译器把 `co_await` 变成一个 awaitable 的三段式调用，并在必要时
把当前协程帧挂起，把控制权交还给 resume 它的人。

展开顺序：

1. 求值 `expr`，得到 awaiter：
   - 类型有 `operator co_await()` → 调它；
   - 或 ADL 的 `operator co_await`；
   - 否则 expr 自己就是 awaiter。
2. 调 `awaiter.await_ready()`。
   - 返回 `true`：不挂起，直接调 `await_resume()`，表达式结束。
   - 返回 `false`：进入挂起流程。
3. 调 `awaiter.await_suspend(handle)`，`handle` 是**当前协程**的句柄：
   - 返回 `void`：当前协程挂起，控制权回到 resumer。
   - 返回 `bool true`：挂起，回到 resumer；`false`：不挂起，立刻继续。
   - 返回 `coroutine_handle<>`：**对称转移**，直接 resume 那个协程（不回到 resumer，
     不增长调用栈）。
4. 协程被恢复后，调 `await_resume()`，它的返回值就是 `co_await` 表达式的值。

**本项目**：`BlockSyscall` 用 **bool** 语义（`block_syscall.hh:19-28`）；
`std::lazy` 的 `_Awaiter` 用 **coroutine_handle** 语义做对称转移（`lazy.hh:492-497`）。

**注意一个容易答错的细节**：`await_suspend` 返回 `false` 时，协程并没有挂起，
但 `await_resume()` **仍然会被调用**。所以 `BlockSyscall` 里"试探成功"的路径是：
`await_suspend` 调一次 syscall → 返回 false → `await_resume` 直接取 `returnValue_`，
**不会重复调用**。只有 `haveSuspend_ == true` 时 `await_resume` 才重试。

### Q17. promise_type 需要哪些成员？

最小集合（取决于协程形态）：

| 成员 | 作用 | 本项目 |
|---|---|---|
| `get_return_object()` | 构造返回给调用者的对象 | `lazy.hh:527` 返回持有 handle 的 `lazy` |
| `initial_suspend()` | 协程开始前是否挂起 | `std::lazy` 是 `suspend_always`（惰性启动）；`oneway_task` 是 `suspend_never`（立刻跑） |
| `final_suspend()` | 结束后是否挂起 | `std::lazy` 返回 `_Final_awaiter`，恢复 `_Cont`（对称转移回调用者）；`oneway_task` 是 `suspend_never`，帧自动销毁 |
| `return_void()` / `return_value()` | 返回值写入 | `lazy<shared_ptr<Socket>>` 用 `return_value`；`lazy<>` 用 `return_void` |
| `unhandled_exception()` | 捕获协程内异常 | `lazy` 存进 `exception_ptr`（`lazy.hh:316-330`），`co_await` 时 rethrow |
| `await_transform()` | 可选，改写 co_await 的表达式 | **本项目没用** |

**`std::lazy` 的 promise 细节**（被问"你读过这层吗"时可以答）：promise 里有一个
union（`_Data` / `_Except`）加 `_Discriminator`，要么存返回值要么存 `exception_ptr`；
`_Cont` 存"恢复我的人"的句柄，`final_suspend` 时返回它完成对称转移。

### Q18. 三个 awaitable 函数的语义？await_suspend 三种返回值？你用了哪种、为什么？

（展开见 Q16，这里只答取舍）

- **`await_ready` 恒 false** 是本项目的核心选择：不做"就绪检查"，而是**直接尝试
  一次系统调用**。系统调用本身就能告诉你"有没有数据"。
  - 好处：省掉一次多余的准备/查询；成功时零挂起开销。
  - 代价：每个**真正会阻塞**的操作多一次注定 `EAGAIN` 的 syscall。实测
    `recvfrom / 请求 = 2.00`，`sendto / 请求 = 1.00`（发通常直接成功）。
- **`await_suspend` 返回 bool**：因为"这次 syscall 成功/失败"本来就是二值结果，
  用 bool 最直观；不需要对称转移（这里不需要"挂起时直接切到另一个协程"，只需
  回到事件循环）。
- **`await_suspend` 里发起 syscall** 是刻意为之：把"尝试 + 注册"合并到一个阶段，
  避免"先查 ready、再查、再调用"的重复。代价是 awaiter 必须活到恢复之后——
  它是 `co_await` 表达式里的临时量，生命周期恰好覆盖挂起期，存放在协程帧里。

**追问"为什么不用 symmetric transfer 挂起"**：`BlockSyscall` 的目标是"挂起后回到
epoll 循环"，而不是"切到另一个协程"；返回 bool 让控制权自然回到 `resume()` 的
调用点，语义更清楚。

### Q19. 协程帧在哪分配？局部变量存哪？co_await 后引用会失效吗？有没有做堆消除/内存池？

- **帧分配**：编译器生成的 `promise_type::operator new`。`std::lazy` 的默认
  `_Allocator = void` 走 type-erased 版本，每次 `::operator new[]` 分配
  `帧大小 + 一个 dealloc 函数指针`（`lazy.hh:158-164`）；支持 `allocator_arg`
  传入自定义分配器（本项目**没用**）。
- **局部变量**：协程的局部变量（包括 `char buffer[4096]`、awaiter 临时量、
  按值捕获的 `shared_ptr`）都在**协程帧**里，不是机器栈。所以挂起后它们仍然有效，
  恢复后还能用；反过来，帧什么时候销毁由 `lazy` 的析构决定，不是函数返回就没了。
- **引用失效的经典场景**：把局部变量的引用/指针传给了会活得更久的对象（比如把
  `std::string&` 丢进异步队列），帧销毁后引用悬空；或者对 `co_await` 的临时
  awaiter 取引用传给别处。本项目里 `Socket*` 是裸指针，靠 `shared_ptr<Socket>`
  保证 socket 活到协程结束；`echo_socket` 按值接 `shared_ptr`，避免了这个问题。
- **帧消除/内存池**：**没有做**。没有 HALO（GCC 在这类对称转移 + 不定大小的帧上
  基本不做），也没有自定义 allocator/帧池。每个 accept 一条新连接 = 至少一次
  handler 帧分配 + 一次 `spawn` 包装帧分配 + 一次 `Socket` 分配 + 一次
  shared_ptr 控制块分配。**这些都在建连路径上，不在每请求路径上** —— 每请求
  不分配帧（handler 帧复用），但会分配 `std::string` 和复制数据。

### Q20. 协程怎么和 epoll 联动？事件到达后谁 resume、在哪个线程？

**一句话**：`await_suspend` 把 `coroutine_handle` 存进 `Socket` 的槽位；事件循环
在自己的线程里调 `Socket::resumeRecv()/resumeSend()`。

- 注册：`await_suspend` → `socket->coroRecv_ = handle`（`socket_recv_operation.cc:34`），
  同时操作对象的构造/析构负责 `watchRead/unwatchRead` 的**关注集**变化。
- 触发：`IOContext::run()` 从 `epoll_pwait` 拿到 `data.ptr` → `Socket*` →
  `resumeRecv()` → `coroRecv_.resume()`。
- **线程**：永远在事件循环线程，整个项目只有一个线程；没有跨线程 resume，
  所以不需要内存序/锁，也没有"在信号处理函数里 resume"的问题（信号是 signalfd，
  在同一个 loop 里以普通 fd 事件的形式处理）。
- 恢复后：`await_resume` 重试 syscall；返回给业务代码。

**追问"如果多线程呢"**：fd 必须注册在它归属的 loop；跨线程 resume 要经过目标
loop 的队列 + eventfd 唤醒，且要保证 `Socket` 生命周期由该 loop 管理（否则
`epoll_ctl(DEL)` 和 `close` 会在错误线程执行）。

### Q21. 生命周期与悬垂句柄：连接断开时挂起的协程怎么办？怎么取消？怎么防 UAF？

**本项目的机制**：

1. **正常断开**：`recv` 返回 0 → handler `co_return` → 帧销毁 → `~Socket` →
   `epoll_ctl(DEL)` + `close`。此时没有挂起的操作。
2. **关停（Ctrl-C）**：`cleanIO()`（`io_context.cc:126-134`）遍历
   `processedSockets_`，对每个 socket `cancel()`：置 `canceled=true`，然后 resume
   挂在 `coroRecv_/coroSend_` 上的句柄。各操作的 `syscall()` 开头有
   `if (socket->canceled) throw`（如 `socket_recv_operation.cc:25`），异常沿协程栈
   展开，操作临时量析构 → `unwatchRead`，最终回到 `spawn` 的 catch。
3. **延迟析构**：异常路径的 `lazy` 被推进 `delayDestructor_`，在事件分发循环
   **之外**统一销毁，避免在遍历/分发中析构（`io_context.cc:73-79`）。

**防 UAF 的关键约定**：

- `epoll_event.data.ptr` 永远指向 `Socket`，不指向 awaiter；awaiter 是临时量。
- 每个连接由 `shared_ptr<Socket>` 持有；handler 帧里的 `shared_ptr` 保证 socket
  活到协程结束。
- 不要在循环里销毁 socket：`detach()` 从 `processedSockets_` 移除，`cleanIO`
  先 cancel 再清 `delayDestructor_`。

**要主动承认的漏洞**：

- `coroRecv_/coroSend_` 恢复后**不置空**（`socket.hh:28-42`），槽位里是悬垂句柄；
  当前靠"同一方向串行、不重复 resume"没炸，但应该置空。
- 同一个方向只有一个槽位，**第二个并发操作会覆盖第一个句柄**，第一个协程永久泄漏
  （`coroRecv_` 更是 recv 和 accept 共用）。
- 正常完成路径（非异常）里，handler 协程帧是在 `resume()` 内部销毁的，
  `~Socket` 也在事件分发循环内执行 `epoll_ctl(DEL)`。当前 `events` 是已经拷贝出来的
  数组，所以没有迭代器失效，但这和 README 里"绝不在分发循环内销毁"的说法有出入——
  真正被延迟的只有**异常路径**。面试时别说"所有销毁都延迟"，会被抓。
- C++20 没有取消原语；本项目的取消只能覆盖"挂在 syscall 上"的协程，如果以后挂在
  定时器/队列上，需要扩展 `cancel()` 的覆盖范围。

### Q22. 异常怎么传播？`noexcept` 与协程有什么坑？

- 协程内抛出的异常（包括 `syscall()` 里 `throw runtime_error`）由编译器捕获，
  调 `promise.unhandled_exception()`。
- `std::lazy` 把它存成 `exception_ptr`（`lazy.hh:316-330`），在 `co_await` 的
  `await_resume()` 里 `rethrow_exception`（`lazy.hh:499-505`），于是异常的抛出点
  变成调用者的 `co_await` 处，可以用普通 `try/catch` 接。
- `spawn` 的包装协程 `catch (const std::exception&)` 把 task 移进
  `delayDestructor_`（`io_context.cc:73-79`）。
- **坑 1**：`catch (const std::exception&)` 接不住非 `std::exception` 派生类型；
  那种异常会走到 `oneway_task::unhandled_exception()`（空实现），**被静默吞掉**。
- **坑 2**：`unhandled_exception()` 自己不能抛（否则 terminate）；`final_suspend`
  必须 `noexcept`；`return_value`/`await_resume` 的 `noexcept` 标注要谨慎，
  里面调用了可能抛的东西就会 terminate。
- **坑 3**：异常的代价是"存到堆上（`exception_ptr` 内部引用计数）+ 恢复时 rethrow"，
  高频错误路径（比如用异常做流控）会很贵。当前 `canceled` 用异常展开是可读性选择，
  不是性能最优。

### Q23. 协程 vs 回调：省了什么，代价是什么？

- **省掉**：手写状态机/状态字段、上下文对象、回调分层、错误逐层透传；
  挂起点的局部变量天然保留（在帧里），不用打包成 context。
- **代价**：
  - 每个协程帧一次堆分配（除非做帧池/allocator）；
  - 调试器栈回溯看不到"挂起链"（帧是堆上的、`resume()` 调用栈不连续）；
  - 没有内置取消、没有 `when_all`/`when_any`、没有超时；
  - 异常路径和帧生命周期要自己设计（延迟析构、句柄置空）；
  - 编译产物更大、编译更慢；`-O0` 下对称转移可能退化成真实递归（本项目实测过，
    见 Q36）。

### Q24. IOContext 调度器具体怎么调度？会忙等吗？协程里做阻塞调用会怎样？

- **调度**：没有就绪队列/任务队列，**epoll 就是调度器**。协程只在两种时刻运行：
  `spawn` 时同步跑到第一次挂起，或 fd 就绪时被 `resume()`。
- **不会忙等**：`epoll_pwait(fd_, events, max_events, -1, &mask)` 超时 `-1`，
  没有事件就永远睡；不设周期性超时，因为**没有定时器**。代价是"空闲超时/心跳"
  这类需求没法实现，必须引入 timerfd 或给 wait 一个超时。
- **单线程无锁**：所有状态只在 loop 线程访问，`processedSockets_`、句柄槽、
  协程帧都不需要原子操作。
- **阻塞调用污染**：如果有人在这个线程里 `recv` 一个阻塞 fd、做磁盘 IO、或者用
  `std::future::get()`，整个循环就停了。本项目的 `recv/send/accept` 全部非阻塞；
  但 handler 里如果有 CPU 长任务，同样会阻塞所有连接（单线程的固有代价）。
- `max_events=10` 意味着"一次最多处理 10 个就绪 fd"，**它是调度器的吞吐上限之一**。

### Q25. 10K 连接 = 10K 协程帧，内存多少？和状态机比谁省？

**实测**：1 万连接 RSS 47.5MB，约 **4.7KB/连接**；但扣掉进程基线（1k 时 8.6MB）
后更接近 **4.0KB/连接**。

**构成（从代码反推）**：

| 项 | 大小 | 说明 |
|---|---|---|
| handler 协程帧里的 `char buffer[4096]` | ~4KB | **绝对大头**（`test_server.cc:22`） |
| 协程帧头部（promise、参数、保存的寄存器/续体） | ~100-200B | |
| `Socket` 对象 | ~80B | 含 2 个 handle、fd、状态位、`addr_res` |
| `shared_ptr` 控制块（非 `make_shared`） | ~32-48B | 独立堆分配 |
| `processedSockets_` 的 `std::set` 节点 | ~40B | 每个 socket 一个节点 |
| 内核 socket 收发缓冲 | 不计入 VmRSS | 用 `tcp_rmem/tcp_wmem` 控制 |

**和状态机比**：如果手写状态机，你同样需要一个 4KB 的接收缓冲（或者按需分配），
所以**帧本身的开销不是主要问题，buffer 才是**。真正的差异是：

- 协程帧 = 一次 `operator new[]` + 帧里有 promise/续体，约多 100-200B；
- 状态机对象可以内联进对象池/SOA，缓存更友好；
- 协程帧的指针是散落的，遍历时 cache miss 更高。

**降到 1~2KB 的路径**：buffer 改 512B/1KB（典型行协议请求远小于此），超长请求再
动态扩容；`Socket` 用 `make_shared`；`processedSockets_` 换成侵入式链表或
每轮脏集合；必要时给 `lazy` 传一个帧池 allocator。

### Q26. 编译层面：`-fcoroutines`、编译器版本、`coroutine_handle` 是什么？

- **协程支持**：GCC 10+ 在 `-std=c++20` 下默认开启协程（老版本需要
  `-fcoroutines`）；本项目 Makefile 用 `-std=c++20`。`-fcoroutines` 在
  `-std=gnu++17`/`-std=c++20` 的某些组合下才需要显式给，**面试时说"GCC 10+ 的
  `-std=c++20` 自带协程，不需要额外开关"即可**。
- **版本**：Makefile 硬编码 `g++-12`，但本机只有 g++ 15.2；`std::lazy` 参考实现里
  第 245 行的 `_Data_` 拼写在 g++-15 下会因实例化 `lazy<bool>` 的 promise 移动构造
  而报错（g++-12 下这个构造函数没被实例化，所以"碰巧能过"）。这是**工具链差异
  暴露的潜在 bug**，不是 g++-15 的问题。
- **`coroutine_handle`**：本质是"指向协程帧的裸指针的封装"；`coroutine_handle<P>`
  可以通过 `promise()` 拿到帧里的 promise，`handle<>` 是类型擦除版。
  `from_promise()` 依赖 promise 在帧里的偏移；`resume()` 恢复执行，`destroy()`
  析构帧，`done()` 判断是否已到 final suspend。
- **`std::lazy` vs 自研 task**：`std::lazy` 是 WG21 参考实现，惰性启动、对称传输、
  `sync_await`、`[[nodiscard]]`；本项目直接 vendored，没自己造轮子。
- 其它 C++20 特性：项目没用 `std::span`/`std::jthread`/`std::stop_token`；
  这是可以主动指出的"未用尽语言能力"。

---

## 4. 并发、内存与资源管理

### Q27. 线程模型：单线程 reactor 能跑满多核吗？怎么扩展？

- **不能**。单线程最多吃满一个核；短连接吞吐在 c≥100 后锁死在 ~37.7K req/s，
  就是单核饱和的直接证据。
- **扩展方案**：
  1. **SO_REUSEPORT + N 个独立 loop**：每线程一个 `IOContext` + 监听 socket，
     内核按四元组分流；无共享、无锁，推荐。
  2. **accept 线程 + round-robin 分发**：一个线程 accept，把 fd 交给 worker loop；
     需要 eventfd/队列跨线程唤醒，且 `epoll_ctl(ADD)` 必须在目标线程做。
  3. **多线程共享一个 epoll**：会出现"同一个 fd 的事件被多个线程取走"的竞争，
     需要 `EPOLLONESHOT` 或 `EPOLLEXCLUSIVE`（后者只用于监听 fd），复杂度高。
  4. **work stealing**：协程任务可以跨 loop 偷，但 socket 归属不能乱；Seastar
     选择 share-nothing（不偷 socket），只偷纯计算任务。
- **惊群**：多个进程/线程在同一个监听 fd 上 `epoll_wait`，新连接到达时可能全部醒来。
  现代内核用 `EPOLLEXCLUSIVE` 或 `SO_REUSEPORT` 缓解；`SO_REUSEPORT` 是哈希分流，
  不是"醒了再抢"，更可预测。

### Q28. `shared_ptr`/`weak_ptr` 用在哪？有循环引用吗？析构顺序和 fd 关闭时机？

- **用在哪**：连接对象 `std::shared_ptr<Socket>`；accept 返回
  `std::lazy<std::shared_ptr<Socket>>`；handler 按值接收 `shared_ptr`。
- **没有循环引用**：`Socket` 持 `IOContext&`（引用，不拥有）；`IOContext` 持
  `Socket*`（裸指针，且在 `detach()` 时移除）；操作对象持 `Socket*`（裸指针，
  生命周期在 `co_await` 内）。所有权是单向的。
- **`enable_shared_from_this` 继承了但没用到**（`socket.hh:14`）；目前是死重量，
  可以删掉或真正用起来（比如在异步回调里安全地延长生命周期）。
- **析构顺序**：handler 帧里的 `shared_ptr` 是最后所有者 → 帧销毁时释放 →
  `~Socket` 执行 `epoll_ctl(DEL)` → `freeaddrinfo` → `close(fd)`。
- **关闭时机的问题**：
  - 没有 `shutdown(SHUT_WR)`，直接 `close`；如果接收缓冲里还有未读数据，内核可能
    发 RST 而不是 FIN，已经排队但未发出的响应可能被丢弃。
  - 没有 `SO_LINGER` 配置；优雅关闭/半关闭需要额外实现。
  - `~Socket` 里 `close` 之后才打印日志，日志里看不到错误码；生产代码应该先
    `shutdown` 再 `close`，并处理 `EINTR`。

### Q29. RAII 封装：fd、移动语义、禁止拷贝怎么做的？

- `Socket` 的析构负责 `close`，fd 是成员，天然 RAII；拷贝构造 `= delete`，
  移动构造存在（`socket.cc:34-42`）。
- **移动构造有潜在 bug**：它复制了 `fd_`、`io_state_`、`io_new_state_`，但
  epoll 里注册的 `data.ptr` 仍然指向**旧对象地址**；而且没有把 `canceled`、
  `coroRecv_/coroSend_` 搬过去。`Socket` 一旦被 move，epoll 事件会解引用错的
  `Socket*`。当前代码没有实际 move 它（accept 用 `new` 后直接交给 shared_ptr），
  所以没暴露；正确做法是禁止移动，或者 move 后重新 `epoll_ctl(MOD)` 更新 `data.ptr`。
- 其它资源（`addr_res`）用 `freeaddrinfo` 在析构里释放；但构造函数里
  `getaddrinfo` 的返回值**没有检查**，失败时 `addr_res` 可能未初始化/为 nullptr，
  后续解引用会崩。
- `IOContext` 持有 `epoll fd`，但析构函数没写 `close(fd_)`（进程退出无所谓，
  作为库用就是 fd 泄漏）。

### Q30. 数据缓冲用了什么？扩容策略？用 writev 了吗？

- 接收：**协程帧里的定长栈数组** `char buffer[4096]`（`main.cc:50`、
  `test_server.cc:22`）。没有动态扩容、没有环形缓冲、没有 `std::span`。
- 发送：临时 `std::string`，一次 `send` 整个响应；部分发送时用
  `data()+sent` 循环（`main.cc:87-91`），**没有用户态写队列**。
- 没有 `writev`/`sendmsg` 聚合 header 和 body（当前 header+body 拼在同一个
  `std::string` 里，一次 `send` 已经足够）。
- 没有 `readv`/`iovec`、没有 `splice/sendfile`、没有 `MSG_MORE`/`TCP_CORK`。
- 加协议时应该换成每连接一个可增长的 `std::vector<char>`/`std::string` +
  解析偏移，避免每次 `erase(0, n)` 的 O(n) 搬移。

### Q31. 内存碎片/分配器：每连接 new、每请求 new？

- **每连接**：`new Socket` + `shared_ptr` 控制块 + `std::set` 节点 + 协程帧
  （`operator new[]` 一次）+ `spawn` 包装帧。约 5 次分配/连接。
- **每请求**：`std::string req`、`std::string resp`、可能的临时 `std::string word`；
  **没有每请求的协程帧分配**（handler 帧复用）。
- **没有对象池/arena**，也没有 `make_shared`（因为 `Socket` 构造函数是私有的，
  需要额外处理）。
- 优化方向：帧池 allocator（`lazy` 支持 `allocator_arg`）；`Socket` 用 slab/侵入式
  链表；`std::string` 用短字符串优化（SSO 已覆盖大部分响应）+ 复用 capacity；
  `processedSockets_` 用侵入式链表替代 `std::set` 节点。

---

## 5. 性能数字与 profiling（最容易被追着打）

### Q32. 17.9K req/s 是不是偏低？瓶颈在哪？

**先说口径**：这是 **1 万条长连接、50,000 个请求、分 5 轮、每轮 1 万请求同时打**
的突发轮次吞吐；不是持续压测，也不是单请求 echo。同一套代码的短连接（wrk，
c=100~1000）稳定在 **37.7K~38.2K req/s**。

**它确实是 CPU-bound**：perf 测得该轮 `task-clock = 2.669s`，而轮次墙钟 2.79s，
即服务端占了约 96% 的墙钟 CPU。每请求 CPU 从 1k 的 **12.6µs** 涨到 10k 的
**53.4µs（4.2 倍）**，IPC 从 0.66 掉到 0.28，cache-miss 从 3.22% 涨到 29.96%。

**瓶颈排序**（有证据 / 推断要分开说）：

1. **`processedSockets_` 每轮全量扫描（有证据）**：`io_context.cc:57-66` 每轮
   遍历所有曾挂起过的 socket，而 10k 规模下 `epoll_pwait` 被 `max_events=10`
   限制成 5,005 轮 → 约 **5,000 万次** `std::set` 节点 + `Socket` 对象的随机指针
   追逐。这直接解释 cache-miss/IPC 的恶化。
2. **`max_events=10`（有证据）**：5k/10k 下 `epoll_wait/请求` 精确为 0.100，
   即每次只取 10 个事件；轮数被放大 10 倍，又进一步放大了第 1 项的扫描成本。
   两项是**乘性叠加**。
3. **逐操作 `std::cout` 追踪（推断，但强烈建议先验证）**：每个操作构造/析构/
   syscall 都有输出，一次请求约 5~8 行 `operator<<`，包含锁、locale、格式化、
   写文件/syscall。bench 构建**没有关掉**它，所以这个数字是带日志的下限。
   逐项 A/B（编译期宏关掉日志）应该能立刻看到提升——这也是面试里最诚实、
   最容易做的下一步实验。
4. **每请求 2 次 `recvfrom`（有证据）**：`await_ready` 恒 false 的代价。1 次成功 +
   1 次注定 EAGAIN 的试探，多一次 syscall。
5. **`std::string` 分配与拷贝、单线程串行、无 TCP_NODELAY**：量级小一些，
   但都是可优化项。
6. **压测端同机**：客户端与服务端在同一个 WSL2 VM（8 核）里，客户端自身 1~2% CPU，
   墙钟里包含客户端调度干扰；不是主因，但口径要说明。

**不要说的**："协程调度开销大"——协程帧只在建连时分配，每请求不分配帧；
"epoll 太慢"——`epoll_ctl=0`、每轮只取 10 个事件，问题在调度器参数和扫描，
不在 epoll 本身。

### Q33. 5 万请求 / 17.9K ≈ 2.8 秒，是短脉冲还是持续压测？

**短脉冲**，准确说是"每轮 1 万请求同时打、共 5 轮"。

- 压测客户端是 Python asyncio 自研脚本：先分批建立 1 万连接，保持空闲，然后在
  **全部连接上同时**发一次请求，逐条校验响应。
- 1k/5k/10k 三个规模都固定 **50,000 个请求**（轮数 R = max(5, 50000/N)），
  这样每请求的 CPU 成本可比。
- **p50 = 190ms 不是单请求延迟**，而是排队延迟：1 万请求同时进入单线程事件循环，
  平均排队时间 ≈ N/(2×吞吐) ≈ 10000/(2×17901) ≈ 279ms 量级（实测 190ms，
  因为一部分请求在后面的轮次里被更快处理）。
- **局限**：没有测持续稳态吞吐（比如 60 秒、客户端流水线发送）；10k 每个连接只有
  5 个请求，样本偏少。面试时主动说"这是连接规模 + 突发吞吐测试，不是稳态 QPS 基准"。

### Q34. RSS 47.5MB / 4.7KB per connection 怎么测的？包含什么？能降到 1~2KB 吗？

- **怎么测**：服务端启动后在连接建立完毕、空闲保持阶段读 `/proc/<pid>/status`
  的 `VmRSS`，同时用 `/proc/<pid>/fd` 数 fd、`ss` 数连接；三个规模点对比。
- **包含什么**：用户态部分主要是**协程帧里的 4KB 接收缓冲**，加上 `Socket`、
  shared_ptr 控制块、`std::set` 节点、协程帧头部；**内核 socket 缓冲不计入 VmRSS**
  （那是 `sk_buff`，另算）。
- **扣基线**：1k 时 8.6MB、10k 时 47.5MB，差值 38.9MB / 9000 ≈ **4.3KB/连接**；
  47.5/10000=4.7KB 的分母把进程基线也算进去了，严格说每连接约 4.0~4.3KB。
- **能降吗**：能。把 `char buffer[4096]` 改成 512B/1KB（行协议请求通常几十字节），
  长请求再动态扩容，就能到 **1~2KB/连接**；再把 `Socket` 用 `make_shared`、
  `std::set` 换侵入式链表，还能再省几十到一百多字节。

### Q35. cache-miss 3.22% → 29.96%：用什么测的？为什么全量扫描会掉命中率？

- **工具/事件**：`perf stat` 的**通用硬件事件** `cache-references` / `cache-misses`，
  不是 L1 专用的 `L1-dcache-load-misses`，也没有拆分到 LLC。所以只能说
  "通用 cache 事件的 miss 率"，**不要声称是 LLC 或 L1**。
- **为什么掉**：
  1. 每轮事件循环扫 10k 个 `std::set` 节点 + 10k 个堆上 `Socket` 对象；
     `std::set` 节点是逐个 `new` 的，地址不连续，遍历就是指针追逐。
  2. 每个 `Socket` 只有几个字段被访问，但 cache line 是 64B，浪费带宽。
  3. 10k 个连接的 handler 帧（每个 4KB）散落在堆上，请求路径每次访问一个不同帧，
     工作集约 40MB，远超 L2/L3，几乎每次都是冷访问。
  4. 请求数固定 5 万而连接数增加，每请求摊到的扫描次数线性增加 → 命中率随 N 恶化。
- **这就是"全量扫描是最大破绽"的完整解释**：epoll 本身是 O(就绪数)，但外层的
  辅助循环把它退化成 O(每轮全部连接数)，再乘以 `max_events` 放大的轮数。

### Q36. "processedSockets_ 全量扫描"：epoll 的意义何在？是不是退化成 O(n)？怎么优化？

**是的，外层退化了**，要说清楚结构：

- epoll 负责"哪些 fd 就绪"，这部分没有退化；
- 退化的是**每轮结束后的 MOD 应用循环**：它遍历的是"所有曾经挂起过的 socket"
  （集合只在 `detach()` 时删除，`io_context.cc:117-124`），而不是"本轮有变化的
  socket"。
- 复杂度：每轮 O(N)，轮数 ≈ 请求数 / max_events → 总成本 O(N × 请求数 / 10)。
  它还是常数级优化能救的范围，不是算法灾难，但在 1 万连接下已经吃掉大量 CPU。

**优化方案**（按投入产出排序）：

1. **把集合拆成两个职责**：当前 `processedSockets_` 同时承担"延迟 MOD 批次"和
   "关停时取消所有 socket 的注册表"。拆成 `allSockets_`（注册表，侵入式链表）
   + `dirtySockets_`（每轮清空，只放状态变化的）。
2. **每轮结束后清空脏集合**：把 `processedSockets_` 与一个局部 set/vector
   `swap`，遍历完就丢；`watch/unwatch` 只往脏集合里插。
3. **只对 `io_new_state_ != io_state_` 的 socket 调 MOD**（已有该判断，但因为
   集合含全量，判断本身也要遍历全量）。
4. **提高 `max_events`** 到 1024/4096，直接减少轮数（与 1/2 叠加是乘性收益）。
5. **数据布局**：`Socket` 用对象池/slab 连续分配；`std::set` 换侵入式双向链表或
   开放寻址；减少 `shared_ptr` 控制块跳转；热字段（fd、状态位、句柄）放同一个
   cache line，冷字段（addr_res、日志）挪出去。
6. **减少每连接工作集**：4KB buffer 缩小/按需分配，让请求路径的工作集回到 L2。

**预期**：报告里没有做优化后的 A/B，所以**不要编数字**。可以说"按上述 1+4，
扫描从 5000 万次降到几十万次量级，轮数从 5000 降到约 50；配合关日志，应该能
明显接近短连接那 37K 的平台，但需要实测验证"。

### Q37. "零拷贝"这个词慎用——你的项目里到底有没有？

**没有。** 数据路径是：

```
客户端 → 内核接收缓冲 → recv() 拷贝到协程帧 buffer[4096]
       → 构造 std::string（再拷贝/分配）
       → send() 拷贝到内核发送缓冲 → 网卡
```

至少两次用户/内核拷贝，外加一次 `std::string` 构造。可以说的正确表述：

- "没有额外的序列化/中间协议层，响应直接从一个 `std::string` 写出"；
- "没有用 `splice/sendfile/SO_ZEROCOPY/io_uring fixed buffer`，因为当前是
  小消息 request-response，收益有限"；
- "如果要减少拷贝，方向是：用 `readv` 分散读、用 `writev` 聚合写、
  环形缓冲 + `sendmsg(MSG_ZEROCOPY)`，或者将来上 io_uring registered buffers"。

**如果被问"那你的长连接回显是不是可以零拷贝"**：echo 场景可以把收到的 buffer
直接写回（一次拷贝到内核），但本项目没有这么做——它构造了词典响应。

---

## 6. 工程化与稳定性

### Q38. 优雅关闭：SIGINT/SIGTERM 的完整流程？signalfd 还是 self-pipe？信号处理函数里能做什么？

**本项目实际流程**（`io_context.cc:11-69`）：

1. `sigprocmask` 屏蔽 `SIGINT`，用 `signalfd` 把它变成一个可读 fd，加入 epoll。
   **选 signalfd 而不是 self-pipe**：不需要额外 fd 对和 `write`，信号语义由内核
   排队；代价是 Linux 专属（项目本来就 Linux-only）。
2. `epoll_pwait` 返回 `data.fd == sigint_fd` 且可读 → `loop_run = false`。
3. 退出循环 → `cleanIO()`：
   - 遍历 `processedSockets_`，逐个 `cancel()`（置标志 + resume 挂起句柄）；
   - 各操作 `syscall()` 抛异常 → 协程栈展开 → 未完成的 `lazy` 进 `delayDestructor_`；
   - 最后 `delayDestructor_.clear()` 销毁所有帧和 socket。
4. 日志里能看到 `accept canceled` → `close(4)`，进程退出。

**这个流程的不足（主动说）**：

- 只处理 `SIGINT`，**没有 `SIGTERM`**（容器/`systemd` 停机用的是 SIGTERM）。
- 没有"停止 accept → 排空 in-flight → 关闭"的渐进阶段：直接取消所有挂起操作，
  正在写的响应会被丢弃（不是优雅排空）。
- 没有超时兜底：如果某个协程不响应 cancel，进程会卡住（当前所有挂起点都是 syscall，
  所以能响，但这是强假设）。
- 信号处理函数里只做 `write`/`sigprocmask` 是 async-signal-safe 的；本项目根本没写
  信号处理函数，全靠 signalfd，**这是更安全的做法**。
- `epoll_pwait` 返回 -1 时没有区分 `EINTR`，直接抛异常退出；正确做法是
  `if (errno == EINTR) continue;`。

### Q39. SIGPIPE 怎么处理？EINTR/EAGAIN/ENOBUFS 呢？

**这是一个真实的缺口：代码里没有处理 SIGPIPE。**

- `send()` 没有 `MSG_NOSIGNAL`，进程也没有 `signal(SIGPIPE, SIG_IGN)`。
  如果对端已经 RST，`send` 会返回 `-1/EPIPE` **并给进程发 SIGPIPE**，
  默认动作是终止进程。压测里没触发是因为请求-响应时序很紧、且客户端正常读；
  生产环境里一个野蛮断开的客户端就能打挂服务端。
- **修复**：启动时 `signal(SIGPIPE, SIG_IGN)`，或者每次 `send` 用
  `MSG_NOSIGNAL`（推荐，作用域更小）。
- **EINTR**：`epoll_pwait`/`accept`/`recv`/`send` 都可能被信号打断。`BlockSyscall`
  只把 `EAGAIN/EWOULDBLOCK` 当"挂起"，`EINTR` 会被当成失败返回给业务层，
  可能被误判为连接错误。应该把 `EINTR` 视为"重试"（不挂起，直接再调一次，
  或者在 `await_resume` 里重试）。
- **ENOBUFS/ENOMEM**：send 返回这种错误时通常应该关闭连接/退避，不能当 EAGAIN。
- **ECONNRESET/EPIPE**：正常关闭路径，应该静默清理，不要抛到顶层。
- **EMFILE**：见 Q11，当前会导致 accept 循环退出，是最严重的一条。

### Q40. 日志：同步还是异步？对性能影响多大？有没有采样/分级？

- **同步 `std::cout`**，每个操作 3~8 行（构造、析构、syscall、await_resume），
  没有日志级别、没有采样、没有条件编译开关。
- **影响**：每行都要拿 `std::cout` 的锁（单线程也有一次原子/锁开销）、走
  `num_put`/locale、格式化 fd/长度，最后 write 到重定向文件。一次请求 5~8 行，
  在 53µs/请求的成本里占相当比例。**bench 构建没有关掉它**，所以 17.9K 是带
  追踪的下限。
- **改进**：编译期宏（`#ifdef TRACE`）或运行期级别；异步日志（每线程 ring buffer +
  flusher 线程）；对高频路径只保留计数器，采样打印；结构化日志（避免
  `ostringstream` 动态分配）。
- **面试加分**：主动说"我的数字里包含调试追踪，所以真实上限更高；下一步先做
  关日志的 A/B"，比硬解释 17.9K 可信得多。

### Q41. 可观测性：暴露哪些指标？怎么采集？

**当前没有内置指标**，只有外部观测手段（报告里用的）：

- `ss -lnt` / `ss -tn state established`：连接数、accept 队列 `Recv-Q`、
  backlog `Send-Q`（本项目恒为 8）。
- `/proc/<pid>/fd`：fd 数（精确反映连接泄漏）。
- `/proc/<pid>/status` 的 `VmRSS`：每连接内存。
- `/proc/<pid>/stat`：utime/stime，算 CPU 占比。
- `nstat`：`TcpExtListenOverflows`、`ListenDrops`、`TCPSynRetrans`、
  `TCPAbortOnClose` 等内核计数器。
- `perf stat`：cycles/instructions/IPC/cache-misses/上下文切换 + syscall tracepoint。

**生产化建议**：进程内维护原子计数器（当前连接数、累计 accept、累计请求、
错误分类、`epoll_wait` 轮数、就绪事件数、平均处理耗时直方图），通过
Prometheus exporter / 定期日志暴露；`ListenOverflows`、`EMFILE`、`SIGPIPE`
错误率必须告警。

### Q42. 单元测试/集成测试怎么写？有没有做过模糊测试/异常注入？

**现状：没有测试框架、没有 CI、没有 `make test`。** 这是这个项目最大的工程短板，
面试时应当承认，并给出方案：

- **单元测试**：测 `BlockSyscall` 可以用一个假 `SyscallOpt`（返回 EAGAIN 的
  mock），验证"试探 → 挂起 → resume → 重试"的状态机；测 `std::lazy` 的
  异常传播/移动语义；测协议 parser 的边界（空行、超长、半包）。
- **集成测试**：起真实服务端 + 客户端，用 ASan/UBSan 构建；覆盖正常请求、
  客户端提前 close、半包、慢客户端、并发 accept、Ctrl-C 关停。
- **异常注入**：`LD_PRELOAD` 拦截 `recv/send/accept` 返回 EAGAIN/EMFILE/EINTR；
  或者用一个可注入错误的 `SyscallOpt` 模板参数；用 `SO_LINGER{1,0}` 制造 RST；
  用不读数据的客户端制造背压；用信号制造 EINTR。
- **模糊测试**：对 HTTP/行协议 parser 做 libFuzzer（把 recv 输入当 fuzz 目标），
  重点查越界和无限循环。
- **压测回归**：把 `BENCHMARK.md` 的脚本固化，记录吞吐/延迟/内存基线，改动后对比。

### Q43. 构建与部署：Makefile/CMake、sanitizer、压测脚本、Docker？

- **构建**：只有 Makefile，硬编码 `CXX=g++-12`；`CXXFLAGS = -Wall -Wextra -Werror
  -pedantic -std=c++20 -g -fsanitize=address`，**默认没有 `-O`**。
  - `-O0` + ASan 构建在连接风暴下会栈溢出（见 Q44），**不能用于压测**。
  - 压测需要 `CXXFLAGS="... -O2 -g"` 覆盖，构建一个不带 ASan 的 `bench` 变体。
- **没有 CMake**，没有安装/打包规则，没有 Dockerfile，没有 CI。
- **部署**：单二进制、监听 8080（`main.cc` 硬编码），无配置文件、无 daemon 化、
  无 systemd unit；`ulimit -n` 需要在启动脚本里显式抬高。
- **改进**：CMake target（`app` / `bench` / `sanitize` / `test`）；CI 里跑
  ASan+UBSan+clang-tidy；Dockerfile 多阶段构建；压测脚本参数化并纳入仓库。

### Q44. `-O0` 下栈溢出是怎么回事？（BENCHMARK 6.4，很值得讲）

**现象**：默认 `-g`（无 `-O`）+ ASan 构建，用 wrk c=50→100 压测，服务端崩：

```
AddressSanitizer: stack-overflow
  io_context.cc:96 in IOContext::watchRead(Socket*)
```

栈帧在 `socket.cc:54 Socket::accept()` 和 `main.cc:97 accept()` 之间交替了约
250 层。

**原因**：`std::lazy` 的对称传输依赖编译器把 `await_suspend` 返回句柄的路径做成
尾调用；`-O0` 下不做这个优化，于是"对称转移"退化成真实递归。accept 队列持续非空时，
accept 循环一直不回到事件循环（因为没有 EAGAIN），每接受一个连接就多一层栈，
最终溢出。

**这说明了什么**（面试可讲的深层点）：

- "挂起释放线程"只在系统调用真的返回 EAGAIN 时发生；如果每次都成功，协程可以
  在同一个栈上无限跑。
- 协程不等于"自动让出"：没有显式的让出点，单线程循环会一直占用控制权。
- 修复方向：
  1. 压测/生产用 `-O2`（缓解，不是根治）；
  2. accept 循环每处理 N 个连接主动让出一次（比如 `co_await` 一个 ready awaiter
     或把任务重新 `spawn`）；
  3. 更根本的是让事件循环自己批量 accept、把连接交给独立任务，而不是在 accept
     协程里递归式地 spawn/resume。
- 这也解释了 README 里"控制权能丝滑地弹回 epoll"的**前提条件**：必须遇到阻塞。

---

## 7. 可能让你现场写的东西

### Q45. 手写最小 promise_type + 自定义 awaitable（`async_read`）

```cpp
#include <coroutine>
#include <sys/socket.h>
#include <cerrno>

struct Task {
    struct promise_type {
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }
    };
    std::coroutine_handle<promise_type> h;
    explicit Task(std::coroutine_handle<promise_type> h) : h(h) {}
    Task(Task&& o) noexcept : h(std::exchange(o.h, {})) {}
    ~Task() { if (h) h.destroy(); }
    void resume() { h.resume(); }
    bool done() const { return h.done(); }
};

// 关键点：把 fd 和 handle 存到一个长寿命对象里；这里用回调模拟事件循环注册。
struct AsyncRead {
    int fd;
    void* buf;
    std::size_t len;
    ssize_t n = 0;
    std::coroutine_handle<> waiter;

    bool await_ready() const noexcept { return false; }   // 总是先试一次

    bool await_suspend(std::coroutine_handle<> h) {
        n = ::recv(fd, buf, len, 0);
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            waiter = h;          // 交给 epoll 层保存（真实项目存在 Socket 槽位）
            return true;         // 挂起
        }
        return false;            // 成功或真错误：不挂起
    }

    ssize_t await_resume() {
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            n = ::recv(fd, buf, len, 0);   // 被唤醒后重试
        return n;
    }
};

// 用法
Task echo(int fd) {
    char buf[4096];
    ssize_t n = co_await AsyncRead{fd, buf, sizeof buf};
    if (n > 0) ::send(fd, buf, n, MSG_NOSIGNAL);
}
```

**讲的时候要点出**：`await_ready=false` 的取舍；`await_suspend` 里存 handle；
`await_resume` 重试；`Task` 的移动/析构/初始挂起；真实项目还要做注册/注销和取消。

### Q46. 手写 epoll 事件循环骨架（非阻塞 + ET 循环读）

```cpp
int ep = epoll_create1(0);
epoll_event ev{};
ev.events = EPOLLIN | EPOLLET;
ev.data.fd = listen_fd;
epoll_ctl(ep, EPOLL_CTL_ADD, listen_fd, &ev);

epoll_event events[1024];
for (;;) {
    int n = epoll_wait(ep, events, 1024, -1);
    if (n == -1) {
        if (errno == EINTR) continue;
        perror("epoll_wait"); break;
    }
    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        uint32_t e = events[i].events;

        if (e & (EPOLLERR | EPOLLHUP)) { close(fd); continue; }   // 别忘

        if (e & EPOLLIN) {
            // 监听 fd：循环 accept 到 EAGAIN（ET 必须）
            if (fd == listen_fd) {
                for (;;) {
                    int c = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (c == -1) {
                        if (errno == EAGAIN) break;
                        if (errno == EMFILE) { /* 预留 fd + 退避 */ }
                        break;
                    }
                    epoll_event ce{};
                    ce.events = EPOLLIN | EPOLLET;
                    ce.data.fd = c;
                    epoll_ctl(ep, EPOLL_CTL_ADD, c, &ce);
                }
            } else {
                // 普通连接：循环 recv 到 EAGAIN（ET 必须）
                char buf[4096];
                for (;;) {
                    ssize_t r = recv(fd, buf, sizeof buf, 0);
                    if (r > 0) { /* 处理；必要时注册 EPOLLOUT 并缓冲 */ }
                    else if (r == 0) { close(fd); break; }       // 对端关闭
                    else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    else { close(fd); break; }
                }
            }
        }
        if (e & EPOLLOUT) { /* 继续写，直到 EAGAIN，写完就 MOD 掉 EPOLLOUT */ }
    }
}
```

**加分点**：ET 循环到 EAGAIN、`EPOLLERR/HUP` 分支、`accept4` 的
`SOCK_NONBLOCK|SOCK_CLOEXEC`、EMFILE 退避、写完后取消 `EPOLLOUT`、
`max_events` 不要设 10。

### Q47. 写一个协程版"并发拉取 N 个任务再汇总"（没有 `when_all` 怎么办）

C++20 **没有** `when_all`/`when_any`，要自己实现。核心思路：计数器 + 最后一个
完成者 resume 等待者；结果存一个数组，避免共享写。

```cpp
#include <coroutine>
#include <exception>
#include <vector>

struct WhenAll {
    std::size_t remaining;                       // 初始化为 N
    std::coroutine_handle<> waiter = nullptr;
    std::vector<Result> results;                 // 每任务一个槽位，完成时各写各的
    std::vector<std::exception_ptr> errors;

    bool await_ready() const noexcept { return remaining == 0; }

    bool await_suspend(std::coroutine_handle<> h) {
        waiter = h;
        // 关键：先挂上等待者再检查计数，避免"子任务已经全部完成"的漏唤醒。
        return remaining != 0;                   // false = 不挂起
    }

    void await_resume() {
        for (auto& e : errors)
            if (e) std::rethrow_exception(e);    // 第一个异常优先
    }

    // 每个子任务结束时调用且只调用一次（本项目中都在 loop 线程）：
    void childDone(std::size_t i) {
        if (--remaining == 0 && waiter)
            waiter.resume();                     // 最后一个完成者负责唤醒
    }
};
```

**易错点（面试官很可能追这个）**：

- 不要在 `await_suspend` 里做 `fetch_sub`。若子任务先全部完成，计数会先归零，
  之后再减就是下溢；正确顺序是"登记 waiter → 再检查计数"。
- 结果按索引写各自的槽位，不要多个子任务并发 `push_back` 同一个 `vector`。
- 单线程 loop 里不需要原子；跨线程才需要 `atomic` + 内存序，而且**不能直接
  `resume()`**，要把唤醒投递回目标 loop（eventfd/任务队列），否则协程会在错误
  线程上跑。
- 取消/超时要额外引入 `stop_token` 或定时器；`when_all` 本身不负责这些。

异常用 `exception_ptr` 数组单独收集，在 `await_resume` 里统一 rethrow，
比"第一个异常就立刻展开其他子任务"更容易实现，代价是其他任务仍会跑完。

### Q48. 解释一段"协程悬挂/野句柄"的 bug 并修复

**可以拿本项目 Q21 的真实隐患当题**：

```cpp
bool resumeRecv() {
    if (!coroRecv_) return false;
    coroRecv_.resume();   // 恢复后没有置空
    return true;
}
// 第二个同方向 recv：
void SocketRecvOperation::suspend(std::coroutine_handle<> h) {
    socket->coroRecv_ = h;          // 覆盖第一个句柄 → 第一个协程永久泄漏
}
```

**危险场景**：同一个 socket 上两个并发 `recv`（或 recv 与 accept 共用槽位），
第二个覆盖第一个；第一个协程永远不会被 resume，帧泄漏，连接资源也永远不释放。
另一个场景：恢复后句柄仍留在槽位，若同一 fd 的事件再次到来，会对一个已经不在
该挂起点的协程执行 `resume()`。

**修复**：

1. `resume` 后立即置空，并用 `std::exchange` 保证只 resume 一次：

   ```cpp
   bool resumeRecv() {
       auto h = std::exchange(coroRecv_, nullptr);
       if (!h || h.done()) return false;
       h.resume();
       return true;
   }
   ```

2. `suspend` 时断言槽位为空（每个方向最多一个操作），把"覆盖"变成显式错误。
3. 对象被销毁/取消时，先置 `canceled`、清槽位，再 resume；`cancel()` 幂等。
4. 多线程场景再加 generation counter 或 `weak_ptr<State>`，防止"句柄恢复时对象
   已析构"。

### Q49. 现场可能让你解释 `await_ready` 恒 false 的流程

用 Q16 的展开流程 + 本项目的 `haveSuspend_` 状态讲一遍，重点：
`await_suspend` 返回 false 时 `await_resume` 仍会被调用，但不会再调 syscall。

---

## 8. 准备建议与反问

### Q50. 画什么图？怎么口述请求的挂起/恢复点？

一张图，两层：

```
时间 →

客户端       服务端 loop                 Socket 槽位             epoll 关注集
  | connect → accept() 试探成功 ---------> 创建 Socket -----------> ADD IN|ET
  |           spawn(handler)
  |           handler: recv 试探 → EAGAIN
  |                                     coroRecv_ = H1            IN
  | ------------ 请求字节 ------------> epoll_wait 返回 EPOLLIN
  |                                     resumeRecv(H1)            IN
  |           recv 成功 ←----------------+
  |           查词典、构造响应
  |           send 试探成功 / EAGAIN
  |                                     coroSend_ = H2            OUT
  | <----------- 响应 ----------------- epoll_wait 返回 EPOLLOUT
  |                                     resumeSend(H2)            (unwatch OUT)
  |           handler co_return → 帧销毁 → ~Socket → DEL + close
```

再画一个连接状态机：`ACCEPTED → READING ⇄ WRITING → CLOSED`，标出
`coroRecv_/coroSend_` 在哪些状态非空、`EPOLLIN/EPOLLOUT` 在哪些状态注册。

### Q51. "优化故事线"怎么讲？

按报告里的真实链路讲，**前后数据只引用已测到的**：

1. **现象**：短连接能到 37.7K，长连接 10k 只有 17.9K；perf 显示每请求 CPU
   从 12.6µs 涨到 53.4µs，IPC 从 0.66 掉到 0.28，cache-miss 从 3.22% 到 29.96%。
2. **工具**：`perf stat`（cycles/instructions/cache/context-switch + syscall
   tracepoint）、`nstat`、`ss`、`/proc`。
3. **定位**：
   - `epoll_ctl` 系统调用为 0 → 延迟批量 MOD 生效，不是它的问题；
   - `epoll_wait/请求 = 0.100` → `max_events=10` 打满，轮数被放大；
   - 轮数 × `processedSockets_` 全量扫描 = 5000 万次指针追逐 → cache-miss；
   - `recvfrom/请求 = 2.00` → `await_ready=false` 的固定代价。
4. **改动**（建议按这个顺序做，并诚实说"还没做完/做完的是前两项"）：
   - `max_events` 10 → 1024；
   - `processedSockets_` 拆成注册表 + 每轮脏集合；
   - 编译期关闭追踪日志做 A/B；
   - buffer 缩小、`make_shared`、侵入式链表；
   - 多线程 `SO_REUSEPORT`。
5. **结果**：报告已有的是"修 backlog（8→SOMAXCONN）+ 批大小 10 后，卡死 0、
   `ListenOverflows` 从 +4835 降到 +96、吞吐回到 13K~21K"；**性能优化的 A/B
   需要补测**，不要编。

### Q52. 措辞降级表（被追问时不翻车）

| 别说 | 改说 |
|---|---|
| 零拷贝 | 没有额外中间层；仍有一次 `recv` 拷贝 + 一次 `send` 拷贝 |
| 生产可用 | 单机 demo/教学实现；1 万长连接压测通过，缺 SIGPIPE/超时/EMFILE 处理 |
| 支持 10 万连接 | 实测 1 万；结构上 epoll 能扩，但内存/扫描/backlog 需要先改 |
| 高性能 | 单线程 17.9K~38K req/s（带调试日志的构建），瓶颈已定位 |
| 完全无泄漏 | ASan/LSan 下 1 万连接跑完无报告；但 LSan 抓到的是堆泄漏，不覆盖逻辑泄漏（句柄槽） |
| 优雅关闭 | 有 Ctrl-C 驱动的取消/展开；没有 SIGTERM、没有 in-flight 排空 |
| 解决了 C10K | 1 万连接测试通过；"解决"要限定为"该场景下正确性和资源回收验证过" |

### Q53. 反问环节可以问什么？

- 团队网络框架是自研还是 asio/Seastar？为什么这么选？
- 协程在你们生产代码里的落地程度：只用于 IO，还是也用于业务编排？
  取消/超时/`when_all` 是自研还是用库？
- 服务发现、协议编解码、限流/背压这些层怎么和网络层分层？
- 线上可观测性主要看哪些指标？有没有统一的事件循环/线程模型规范？
- 新人接手网络层时，最常见的坑是什么？

---

## 附录 A：本项目"没做"清单（主动坦白用）

| 项 | 现状 | 风险/影响 | 修法 |
|---|---|---|---|
| `listen` backlog | 8（`socket.cc:28`） | 突发建连丢连接、客户端卡死 | `SOMAXCONN` |
| `int opt` 未初始化 | 已修（工作区） | 原 `SO_REUSEADDR` 无效，重启 bind 失败 | `int opt = 1` |
| `lazy.hh` `_Data_` 拼写 | 已修（工作区） | g++-15 下编译失败 | 改 `_Data` |
| SIGPIPE | **未处理** | 对端 RST 时进程被信号杀死 | `MSG_NOSIGNAL` / `SIG_IGN` |
| EMFILE | **未处理** | accept 循环退出，服务端永久不接新连接 | 预留 fd + 退避 + 继续循环 |
| EPOLLERR/HUP/RDHUP | **未显式处理** | 依赖"错误常伴随 EPOLLIN"碰巧能工作 | 显式分支 |
| 空闲超时/心跳 | **无** | 慢/死连接永久占用资源 | timerfd/时间轮 |
| 写缓冲/背压 | 无用户态队列 | request-response 下天然有界；加推送会 OOM | 有界 outbuf + 高低水位 |
| 跨包拆包 | **无累积缓冲** | 一次 recv 当完整请求，半包/粘包会错 | 长度前缀/分隔符 + inbuf |
| 句柄槽置空 | **未置空** | 悬垂句柄/重复 resume 隐患 | `std::exchange` + 断言 |
| 同方向单槽位 | 设计如此 | 第二个并发操作覆盖前一个 | 每方向队列/断言 |
| `Socket` 移动构造 | 有实现但有问题 | epoll `data.ptr` 指向旧地址 | 禁止 move 或重注册 |
| `getaddrinfo` 返回值 | 未检查 | 失败时崩溃 | 检查 + 抛异常 |
| `IOContext` 析构 | 未 `close(epoll_fd)` | 库场景 fd 泄漏 | RAII |
| 构建 | Makefile 硬编码 g++-12、无 `-O`、ASan 常开 | 压测栈溢出；新机器编译失败 | CMake + 构建变体 |
| 测试 | 无单测/CI/fuzz | 回归靠手工压测 | 见 Q42 |
| 多线程 | 无 | 单核饱和 | `SO_REUSEPORT` 多 loop |

## 附录 B：数字速查卡

| 数字 | 值 | 口径/出处 | 一句话解释 |
|---|---|---|---|
| 短连接吞吐 | 37.3K~38.2K req/s | wrk, `main.cc`, c=10~1000, 10s | c≥100 后锁死 → 单线程饱和 |
| 短连接 p50/p99 | ~0.17ms / 0.4~0.7s | wrk `--latency` | p99 是排队长尾 |
| 长连接吞吐 @1k/5k/10k | 20.4K / 28.4K / 17.9K req/s | 自研 asyncio，各 5 万请求 | 10k 下扫描 + cache 恶化 |
| 每请求 CPU | 12.6 / 25.3 / 53.4 µs | `perf stat task-clock` / 50k | 4.2× 退化 |
| IPC | 0.66 / 0.46 / 0.28 | perf | 内存停顿增多 |
| cache-miss | 3.22% / 13.62% / 29.96% | perf 通用 cache 事件 | 不要说成 L1/LLC 专用 |
| `epoll_wait`/请求 | 0.259 / 0.100 / 0.100 | syscall tracepoint | 10k 下每次正好 10 事件 = `max_events` |
| 每次唤醒处理请求 | 3.86 / 9.99 / 9.99 | 同上 | 打满上限 |
| `epoll_ctl` 次数 | 0 / 0 / 0 | 同上 | 延迟批量 MOD 生效 |
| `recvfrom`/请求 | 2.00 | 同上 | `await_ready=false` 的试探代价 |
| `sendto`/请求 | 1.00 | 同上 | 发送通常不挂起 |
| 连接数 | 10000/10000 | 客户端逐条校验 | 全部正确 |
| fd 数 | 10006~10007 | `/proc/<pid>/fd` | 10000 + listen/epoll/signalfd 等基线 6~7 个 |
| RSS | 47.5MB / 4.7KB per conn | `VmRSS`，扣基线约 4.0~4.3KB | 主要是 4KB 协程帧 buffer |
| 线程数 | 1 | `/proc` | 单线程 |
| backlog | 8 | `ss -lnt` Send-Q | 卡死根因 |
| batch=200 溢出 | `ListenOverflows` +4835，卡死 30~62 | `nstat` | 超重传吸收能力 |
| batch=10 溢出 | +96，卡死 0 | 同上 | 内核丢 SYN 后重传能吸收 |
| 建连耗时 @10k | 19~20s（batch=200）/ 94s（batch=10，第二轮） | 客户端计时 | 客户端 TIME_WAIT 累积干扰 |
| ASan/LSan | 无报告 | `-fsanitize=address` | 堆泄漏无；逻辑泄漏不覆盖 |
| 构建 | `-Wall -Wextra -Werror -pedantic -std=c++20 -g -fsanitize=address` | Makefile | **无 `-O`**，压测需覆盖 |
| 折叠 | `-O0` 栈溢出 | ASan 报告 | 对称转移退化递归 |
