# C++ coroutine with epoll

Use C++20 coroutine to have async accept / recv / send operation

Exemple echo server with in the main.cc



# 主要目的
学习协程 封装了一个echo server服务 

# 库文件
需要使用http-parser库解析请求  

1.协程就是可以挂起（Suspend）并在稍后恢复（Resume）执行的函数。 

这个项目使用了C++20提案里面的协程，c++委员会提供的参考lazy.h的头文件，这个头文件主要是做内存管理，状态与异常，对称传输，桥梁同步。

里面的lazy_promise_base是控制中心，主要作用是去堆上分配一块内存（协程帧），内部有一个union（联合体），要么装数据、异常。
Lazy类里面有一个指针，指向协程帧。
coroutine_handle表面是一个类，但底层是一个裸指针，指向协程帧的起始地址，通过指针偏移，
给用户看，用户可以去唤醒或销毁这块内存。
给状态机看，拿promise_type的引用。

对称传输

零开销，当前协程直接、无缝跳转到下一个的协程运行机制。
挂起的时候Awaiter把主调协程A的地址记在子协程的Cont变量里。恢复的时候使用CPU的JMP指令。

桥接同步

引入无操作协程::std::noop_coroutine() 恢复和销毁都是空操作。






2.封装epoll事件驱动，将非阻塞异步回调模型重构成同步流水线工作。



首先是系统调用的协程化，将accep、send、recv等非阻塞系统调用，统一封装为继承BlockSyscall 的 Awaiter 类。

该基类核心实现了 await_ready、await_suspend 和 await_resume 三大核心接口：

await_ready 统一返回 false 以强制触发试探性系统调用；

在 await_suspend 中，若捕获到系统调用返回 -1 且错误码为 EAGAIN 或 EWOULDBLOCK，则判定内核缓冲区未就绪。此时会触发Awaiter 的 suspend 函数，将编译器生成的、代表当前执行现场的**协程帧句柄（std::coroutine_handle<>）**作为接力棒，精准寄存到 Socket 大管家的专属物理卡槽中，随后协程就地冻结，释放 CPU 线程。这些行为最终封装在Socket类中。由于 _Awaiter 对象作为临时变量生命周期极短，为了防止野指针崩溃，项目采用长寿命的 Socket 作为 epoll.data.ptr 的稳固绑定宿主。

Socket 内部额外引入了两个句柄指针：coroRecv_（负责挂起/恢复接收与接入）与 coroSend_（负责挂起/恢复发送），作为中介调度站。

由于频繁与事件循环调度器（IOContext）进行交互，所有的连接对象统一采用 std::shared_ptr 进行生命周期托管，确保异步挂起期间网络流资源的绝对内存安全。



驱动任务与无感知异步化（oneway_task）：
在外层驱动端，引入了 IOContext 调度器，并通过一个返回值为 oneway_task 的匿名就地执行协程，
将用户的业务（std::lazy<>）打包送入循环。

oneway_task 其初始与结束状态均配置为永不挂起（suspend_never）。

它的作用在于：即使上层业务没有显式的 co_await 唤醒指令，它也能无条件地在后台向前狂奔。
一旦业务线遭遇网络阻塞而挂起，控制权能丝滑地弹回给底层的 epoll_pwait 循环。

状态延迟合并优化与批量延迟清理：
在具体的读写调度中，流水线通过监听 Socket 内部的状态位变化（io_state_ / io_new_state_）来驱动。

为了将高并发下的系统调用次数压榨到极限，引入了暂存队列（processedSockets_），在流水线读写时只标记状态，等到每轮 epoll 事件处理完毕后，再集中执行唯一一次 epoll_ctl(..., EPOLL_CTL_MOD, ...) 延迟批量合并修改。

同样地，为了规避迭代器失效与生命周期冲突，所有断开连接或炸裂的协程任务均不原地销毁，
而是统一推进延迟销毁容器（delayDestructor_），在整轮事件循环的末尾集中清空，完美实现了零泄露的内存闭环。













