# X 项目设计哲学

[English](README.md)

## 为什么要做这个项目

C++11 引入了右值引用，随之而来的是**移动语义**——这个机制表面上看是一个性能优化（避免拷贝大对象），但它实际上要深刻得多。在移动语义出现之前，C++ 只有两种传递值的方式：拷贝，或者传指针。拷贝是安全的但昂贵；指针廉价但危险——类型系统里没有任何东西告诉你谁拥有这块内存、它什么时候会被释放、甚至它是否还有效。这就是为什么 C++ 代码库里总是萦绕着"这该谁 free？"和"这指针还活着吗？"之类的问题——这些问题在有 GC 的语言里根本不存在，C++ 开发者只能自己承担，靠 valgrind 跑内存泄漏、靠 ASan 查越界、靠一次次调试定位悬挂指针。

移动语义填补了这个鸿沟。当你对某个值 `std::move` 时，你并不是在拷贝字节——你是在转移**所有权**。源对象被留在一个"有效但未指定"的状态，而目标对象承担全部责任。这是**所有权语义**，而不只是一个拷贝省略的语法糖。配合 RAII 析构，移动语义让你可以在类型系统里表达："我是唯一持有这个资源的人，当我离开作用域时，它就会被清理。"不需要引用计数，不需要垃圾回收，不需要手动 `free`。

Rust 接过了这个理念并把它做成了语言的基础——每个值有且仅有一个所有者，编译器在编译期强制检查借用规则，你不需要运行时就能获得内存安全。C++ 做不到 Rust 那种编译器级别的保证，但能用库来做相当接近的事情。这就是 **libxpp** 的由来：一个 C++11 封装库，利用移动语义实现了 `Own<T>`（单所有者堆分配）、`Box<T>`、`Rc<T>` / `Arc<T>`（共享所有权）、`NonNull<T>`（非空指针抽象），以及一整套 Rust 风格的类型：`Option<T>`、`Result<T, E>`、`Variant<Ts...>`。目标是让值语义——特别是移动语义——成为写 C++ 的默认方式，这样代码读起来就是"我有一个值，我把它移动给你"，而不是"给你指针，别忘了释放"。

## 智能指针只是开始

扎实的值类型是必要的，但还不够。一门现代编程语言还需要一个好的**异步 I/O** 叙事。C/C++ 生态不缺事件库——libevent、libev，以及我个人最爱的 libuv——但这些是*事件通知*库，不是*异步编程*框架。它们告诉你某个 socket 可读了，但不给你 Go 或 Rust 那种体验：写一段看起来是顺序执行的代码，在 I/O 边界处自动挂起和恢复。

要从"事件循环告诉我来数据了"走到"我写下 `co_await socket.read(buf)` 它就自己跑起来了"，需要构建的东西包括：一个能挂起和恢复任务的调度器、一种让协程让出执行权并等待被重新 poll 的机制、一套串联异步操作的标准 API、能并发执行或竞速超时的组合器、与事件循环的 I/O 多路复用的集成，以及——最关键的是——在不丢失类型信息的前提下把错误沿着异步链路传播出去的方式。这些东西，就是区分一个裸事件库和一个异步运行时的"非常多的东西"。

也许有人会问：为什么不直接用 Boost.Asio？Asio 确实是 C++ 生态里最成熟的异步库，但它的问题在于设计时 C++11 还没普及，整个库建立在一个庞大的回调链和 `io_service` 调度模型上，类型系统参与感很弱，错误处理几乎全靠 `error_code`。Coroutine 支持是后期用宏和模板"粘"上去的，而非原生设计。我们想要的是一个从头开始就把类型安全、移动语义和协程作为一等公民来设计的异步栈。

## 无栈 + 有栈：两种协程模型

在运行时层面，异步编程有两个流派：

**有栈协程**（Go、Lua、libco、`xpp::fiber()`）：每个协程有自己的栈（64 KiB，mmap 分配并附带 guard page）。协程在 `.await()` 处阻塞时，运行时通过 `swapcontext` 切换到事件循环栈——不需要修改任何函数签名，不需要编译器支持，C++11 就能用。协程的局部变量放在自己的栈上，所以可以在任意调用深度 `.await()`。

**无栈协程**（Rust、JavaScript、C++20 `co_await`）：协程由编译器编译成一个状态机。每个 `co_await` 点变成一个状态转移。不分配独立的栈——协程的局部变量变成匿名结构体的字段，整个帧在堆上分配（如果编译器能证明不需要则会被优化掉）。每个协程的成本大约是其局部变量的大小加上一个函数指针表。

**libxpp 两种都支持。** 对于 C++11 项目（或者你不想用 `co_await` 给函数"染色"的场景），`xpp::fiber()` 提供了带独立栈的有栈 fiber——写线性 `.await()` 代码，fiber 自动挂起和恢复。对于 C++20 项目，原生 `co_await` / `co_return` 直接在 `Promise<T>` 上工作。二者最终都收敛到同一个 `poll()` 驱动的 `PromiseWaker` 机制——唯一的区别是执行体的挂起/恢复方式不同。

## Promise\<T\> 抽象

Rust 的 `Future` trait 是蓝图：一个异步操作就是一个状态机，被 poll 的时候要么返回 `Poll::Ready(value)`，要么返回 `Poll::Pending` 并注册一个 waker 以便在有进展时被唤醒。执行器通过循环调用 `poll()` 来驱动状态机，直到 future 完成。

C++ 没有这个 trait 作为语言特性，但它给了我们实现它的工具。我们的 `Promise<T>` 是一个具体模板，提供 `poll(waker) → Option<T>` 的 polling 接口——它内部持有一个类型擦除后的协程帧或适配器节点，但从使用者角度看，`Promise<T>` 始终是完整类型的，编译器在每一个调用点都会检查。它同时支持 C++11（通过 `.then()` 回调和 `.await()`）和 C++20（通过原生 `co_await` / `co_return` 协程）。开启 `XPP_FIBER` 后，`.await()` 自动检测是否跑在 fiber 内并使用有栈挂起——同一个 `.await()` 在 `xpp::fiber()` 内外都能用。完整设计见 [Promise 章节](libxpp/promise/)。

当一个 C++20 协程碰到 `co_await` 时，它挂起并把 waker 注册到被等待的子 Promise 上。当那个子 Promise resolve 时，它调用 waker，waker 把挂起的协程排入事件循环等待重新 poll。这就是驱动 `tokio` 的核心机制，只不过我们是在库层面实现的，而不是在语言运行时里。完整设计见 [Promise 章节](libxpp/promise/)。

## 构建异步技术栈

以 `Promise<T>` 为基础，我们可以构建 Rust 开发者熟悉的那套异步模块：

- **I/O 工具**：`BufReader`/`BufWriter` 做高效缓冲读写，`io::copy()` 和 `io::read_all()` 处理常见模式，`Duplex`/`Simplex` 做进程内通信
- **网络**：`TcpStream` 做异步 TCP，`TcpListener` 接受连接，`UdpSocket`、DNS 解析、基于 OpenSSL 或 mbedTLS 的 TLS
- **文件系统**：带游标追踪的异步 `File`、`stat`、目录操作
- **Channel**：`oneshot`（单值）、`mpsc`（有界和无界）、`broadcast`（多消费者 + 滞后检测）、`watch`（版本追踪的最新值），外加 `Notify` 做纯粹的信号通知

我们在 API 设计上刻意同时对齐 STL（命名惯例、迭代器模式）和 Tokio（channel 语义、异步方法签名、错误类型）。Rust 开发者看到 `mpsc::channel<int>(cap)` 应该会感到熟悉，C++ 开发者看到 `rx.recv()` 和 `tx.send(v)` 也不会有距离感。这不是一个附加的偏好，而是一个设计约束。

## 底层基础：libx

所有这一切都运行在 **libx** 之上，一个 C99 库，提供了事件循环、非阻塞 I/O、定时器、无锁 MPSC 队列，以及（自 `xbase/fiber.h` 起）跨平台有栈 fiber（`xFiberCreate` / `xFiberSwitch`）。libx 建基于同样的理念：给 C 开发者一个开箱即用的异步运行时——没有回调地狱，不需要手动管理 fd，只需要 `xTcpConnect()` 和一个 promise 式的回调。libxpp 是在此之上的 C++ 层，增加了类型安全、移动语义、`.await()` 易用性，以及 `xpp::fiber()` 集成——名字本身就是这个关系：底层 C 库叫 **libx**，上层 C++ 库自然就叫 **libxpp**。

## 走进设计

如果上面的内容让你想进一步了解每个模块的细节，以下是各部分的入口：

- **[类型系统](libxpp/smart-pointers/)** — `Own<T>`、`Box<T>`、`Rc<T>`、`Arc<T>`、`NonNull<T>` 如何在库层面实现 Rust 风格的所有权，以及和编译器强制 borrow checker 相比的边界在哪
- **[Promise 模型](libxpp/promise/)** — poll-waker 状态机，`.await()` fiber 挂起和事件循环驱动，C++20 协程帧如何映射到 `Promise<T>`，串联、取消和错误传播的内部机制
- **[异步 I/O](libxpp/io/)** — 从原始 `AsyncFd` 往上经过 `BufReader`/`BufWriter` 到类型安全的 `TcpStream` 和 `File` 的分层架构，以及 `io::copy`、`Duplex`/`Simplex` 等工具
- **[Channel](libxpp/channels/)** — 完整的 Tokio 对齐套件：`oneshot`、`mpsc`（有界用无锁环形缓冲区，无界用无锁链表）、带滞后恢复的 `broadcast`、版本追踪"已读"语义的 `watch`，以及可复用的唤醒原语 `Notify`
- **[线程模型](libxpp/promise/#thread-safety)** — `XPP_MT` 编译开关将 `Shared<T>` 从 `Rc` 切换为 `Arc`，`loom` 模块提供可替换的并发原语用于未来的并发测试，以及所有 channel 的 RAII close 语义
- **[Network](libxpp/net/)** — 异步 TCP、UDP、DNS、TLS，全部建立在同一个 `Promise<T>` 基础上
- **[Filesystem](libxpp/fs.md)** — 异步文件 I/O，带游标追踪，支持 stat、目录操作等
- **[Time](libxpp/time.md)(TODO)** — Tokio 风格的时间原语 — `Instant`、`Duration`、`sleep`、`interval`、`timeout` — 全部基于 `Promise<T>`

贯穿始终的设计哲学是同一个：善用 C++ 已经给我们的东西（移动语义、RAII、协程代码生成），构建一个用起来像 Rust + Tokio 的异步体验，底层跑在 C 的基础上，能融入现有 C++ 项目而无需语言分支或自定义编译器。

---

最终形成的技术栈让 C 和 C++ 各自获得它们应得的异步体验：libx 给需要结构化并发的系统程序员，libxpp 给可以选用 `xpp::fiber([]() { auto v = promise.await(); ... })`（C++11）、`Promise<T>::then([](auto v){...})`（回调链）或 `co_await promise`（C++20 协程）的应用开发者——没有裸指针，没有泄漏的抽象，没有回调金字塔。
