一句话版本
用 C++20 协程把 Linux 的非阻塞 I/O 系统调用（accept / recv / send）封装成 co_await 表达式，让用户写出「看起来像阻塞同步」的代码，但底层线程从不真正阻塞——线程在 I/O 未就绪时挂起协程、交还给 epoll 事件循环。

解决了什么问题（核心，先说清楚）
传统非阻塞服务器的痛点：回调地狱 + 状态机爆炸。

写 epoll 非阻塞服务器，要么：

回调模型：读一半数据、写一半数据，都得把「剩下没干完的活」打包成回调，一个简单的 HTTP 请求解析要拆成十几个 callback，逻辑支离破碎，错误处理（连接中途断开）要到处写。
阻塞 + 多线程：代码直白，但每个连接一个线程，高并发下线程切换、栈内存都是开销。
这个项目的答案是第三条路：协程。

用户代码是直线式的：n = co_await socket.recv(buf)、co_await socket.send(...)，看起来就是阻塞代码。
但 co_await 遇到 EAGAIN 时，挂起的是协程而不是线程——线程被释放，回去继续跑 epoll 循环。
于是一个线程就能驱动成千上万个连接，同时代码又和同步阻塞一样好读。
一句话：用协程把「异步的复杂度」和「同步的可读性」同时拿到了。

面试怎么介绍（建议的结构，3 分钟）
第一步：一句话定调
见开头「一句话版本」。

第二步：讲架构分层（从下往上，讲给面试官听你理解了整体）
block_syscall.hh 是 CRTP awaiter 基类，三个 I/O 操作复用同一个模式
三个 Operation（accept / recv / send）是 co_await 处构造的临时 awaiter
Socket 是 epoll 事件的稳定宿主（data.ptr 指向它，不是 awaiter）
IOContext 是 epoll 事件循环
第三步：讲最关键的机制（区分度，面试官会竖耳朵）
重点讲三个「设计点」，每个都是能深挖的点：

1. 系统调用为什么在 await_suspend 里发起，而不是 await_resume？

故意让 await_ready() 恒返回 false，强制先「试探性」调一次系统调用。这样省掉一次多余的就绪检查，配合 EPOLLET 边沿触发：一次就绪通知驱动协程一直跑到排空或再次阻塞为止。

2. epoll 为什么绑定 Socket 而不是 awaiter？

awaiter 是 co_await 处的临时量，表达式结束就销毁，拿它当 epoll 标识会野指针。Socket 是长寿命的、用 shared_ptr 托管的，才是稳定桥梁。

3. 延迟批量 epoll_ctl 和延迟销毁

watch / unwatch 不直接碰 epoll，只改状态位、记进一个 set，一轮事件分发完再统一 EPOLL_CTL_MOD 一次——把高并发下的系统调用次数压到最低。同理，协程 / socket 不在事件分发循环内销毁，统一延迟到循环末尾清理，避免迭代器失效和 use-after-free。

第四步：收尾点一句「还没做完 / 可以改进的地方」（展示判断力）
比如每个 Socket 每个方向只有一个协程槽位，同一方向不能有两个并发挂起操作；或者 test_server.cc 还有没解决的合并冲突。

准备好被追问的深度点（大概率会考）
追问	你要能答
std::lazy 怎么实现挂起 / 恢复的？	WG21 D2506R0 参考实现，initial_suspend = suspend_always 惰性启动，promise 帧里 union 装返回值或 exception_ptr，对称传输（挂起时把主调协程地址记进子协程的 continuation）。
边沿触发 EPOLLET 为什么要配合「在 await_suspend 里发起 syscall」？	边沿触发只通知一次，必须保证一次通知就把缓冲排空；试探性调用 + 挂起重试正好契合这个假设。
为什么用 CRTP 不用虚函数？	编译期多态、零虚表开销；BlockSyscall 里用 static_cast<SyscallOpt*>(this)。
关停怎么做的？	阻塞 SIGINT、signalfd 进 epoll，cleanIO() 时 cancel() 置标志位恢复协程，协程在 syscall 开头 throw 展开栈。
内存安全怎么保证的？	shared_ptr + ASan + 延迟销毁；[[nodiscard]] 防 task 泄漏。
一句话总结给面试官
这是一个用 C++20 协程 + epoll 手写的异步服务器，核心思想是把非阻塞 I/O 的挂起点从「线程」转移到「协程」，从而在单线程下用同步写法承载高并发。
