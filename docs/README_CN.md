# X 项目设计哲学

[English](README.md)

## 为什么要做这个项目

C++11 引入了右值引用，随之而来的是**移动语义**——这个机制表面上看是一个性能优化（避免拷贝大对象），但它实际上要深刻得多。在移动语义出现之前，C++ 只有两种传递值的方式：拷贝，或者传指针。拷贝是安全的但昂贵；指针廉价但危险——类型系统里没有任何东西告诉你谁拥有这块内存、它什么时候会被释放、甚至它是否还有效。这就是为什么 C++ 代码库里总是萦绕着"这该谁 free？"和"这指针还活着吗？"之类的问题——这些问题在有 GC 的语言里根本不存在，C++ 开发者只能自己承担，靠 valgrind 跑内存泄漏、靠 ASan 查越界、靠一次次调试定位悬挂指针。

```cpp
// 三种把 buffer 传给另一个函数的方式：

// 1. 拷贝 — 安全，但会深拷贝上 GB 的视频帧数据。
void process_copy(std::vector<uint8_t> buf);  // 调用方知道 buf 被拷贝了

// 2. 裸指针 — 廉价，但这块内存谁持有？谁来释放？
void process_ptr(uint8_t* data, size_t len);  // 调用方："data 还有效吗？"

// 3. 右值引用 — 廉价且语义清晰。"我用完了，归你了。"
void process_move(std::vector<uint8_t>&& buf); // 调用方：std::move(buf)
                                               // 被调方：独享所有权，RAII 自动清理
```

传统 C++ 要么靠拷贝（大对象太贵），要么靠指针（所有权模糊）。两者都无法
在函数签名里编码"谁持有这个值"。

移动语义填补了这个鸿沟。当你对某个值 `std::move` 时，你并不是在拷贝字节——你是在转移**所有权**。源对象被留在一个"有效但未指定"的状态，而目标对象承担全部责任。配合 RAII 析构，移动语义让你可以在类型系统里表达："我是唯一持有这个资源的人，当我离开作用域时，它就会被清理。"不需要引用计数，不需要垃圾回收，不需要手动 `free`。

Rust 接过了这个理念并把它做成了语言的基础——每个值有且仅有一个所有者，编译器在编译期强制检查借用规则，你不需要运行时就能获得内存安全。C++ 做不到 Rust 那种编译器级别的保证，但能用库来做相当接近的事情。这就是 **libxpp** 的由来：一个 C++11 封装库，利用移动语义实现了 `Own<T>`（单所有者堆分配）、`Box<T>`、`Rc<T>` / `Arc<T>`（共享所有权）、`NonNull<T>`（非空指针抽象），以及一整套 Rust 风格的类型：`Option<T>`、`Result<T, E>`、`Enum<Ts...>`。目标是让值语义——特别是移动语义——成为写 C++ 的默认方式，这样代码读起来就是"我有一个值，我把它移动给你"，而不是"给你指针，别忘了释放"。

## 智能指针只是开始

扎实的值类型是必要的，但还不够。一门现代语言还需要一个好的**异步 I/O** 叙事。C/C++ 生态不缺事件库——libevent、libev，以及我个人最爱的 libuv——但这些是*事件通知*库，不是*异步编程*框架。它们告诉你某个 socket 可读了，但不给你 Go 或 Rust 那种体验：写一段看起来是顺序执行的代码，在 I/O 边界处自动挂起和恢复。

要从"事件循环告诉我来数据了"走到"我写下 `.await()` 它就自己跑起来了"，需要构建的东西包括：一个能挂起和恢复任务的调度器、一种让出执行权并等待被重新 poll 的机制、一套串联异步操作的标准 API、能并发执行或竞速超时的组合器、与事件循环的 I/O 多路复用的集成，以及——最关键的是——在不丢失类型信息的前提下把错误沿着异步链路传播出去的方式。这些东西，就是区分一个裸事件库和一个异步运行时的"非常多的东西"。

也许有人会问：为什么不直接用 Boost.Asio？Asio 确实是 C++ 生态里最成熟的异步库，但它的问题在于设计时 C++11 还没普及，整个库建立在一个庞大的回调链和 `io_service` 调度模型上，类型系统参与感很弱，错误处理几乎全靠 `error_code`。Coroutine 支持是后期用宏和模板"粘"上去的，而非原生设计。我们想要的是一个从头开始就把类型安全、移动语义和多种 await 方式作为一等公民来设计的异步栈。

## Promise\<T\> 抽象

Rust 的 `Future` trait 是蓝图：一个异步操作就是一个状态机，被 poll 的时候要么返回 `Poll::Ready(value)`，要么返回 `Poll::Pending` 并注册一个 waker 以便在有进展时被唤醒。执行器通过循环调用 `poll()` 来驱动状态机，直到 future 完成。

C++ 没有这个 trait 作为语言特性，但它给了我们实现它的工具。我们的 `Promise<T>` 是一个具体模板，提供 `poll(waker) → Option<T>` 的 polling 接口——它内部持有一个类型擦除后的节点（协程帧、适配器或链），但从使用者角度看，`Promise<T>` 始终是完整类型的，编译器在每一个调用点都会检查。

当你对一个 Promise 调用 `.await()` 时，它进入一个 polling 循环：先尝试 `poll()`，如果值还没就绪，就 park 当前上下文让事件循环推进进度。一旦 Promise 被 resolve——某个 I/O 完成、定时器触发、channel 收到值——waker 被调用，polling 循环解除 park，`poll()` 返回值。这就是驱动 `tokio` 的核心机制，只不过我们是在库层面实现的，而不是在语言运行时里。完整设计见 [Promise 章节](libxpp/promise/)。

## 如何使用

因为一切最终都收敛到 `poll()` 上，`Promise<T>` 支持三种编码风格——每种同等有效，共用同一套底层机制：

### 1. `.await()` —— 任意 C++11 编译器

```cpp
xpp::EventLoop loop;
xpp::WaitScope scope(loop);

int result = fetch_value()              // Promise<int>
    .then([](int x) { return x * 2; })
    .await();                           // 驱动事件循环直到 resolved
```

`.await()` *自己驱动事件循环*——它在一个循环里调用 `xEventLoopRun(X_RUN_ONCE)`，直到 Promise resolve。这是最通用的入口：`main()` 里能用，测试里能用，任何有 `WaitScope` 的地方都能用。

### 2. `.await()` + fiber —— 非阻塞并发

```cpp
xpp::fiber([]() {
    auto a = http_get("/a").await();    // fiber 挂起，事件循环继续
    auto b = http_get("/b").await();    // a 就绪后恢复
    return a + b;
}).then([](int total) {
    printf("total = %d\n", total);
});
```

把代码包进 `xpp::fiber()`，`.await()` 就*自动*变成非阻塞的。fiber 有自己的 64 KiB `mmap` 栈（带 guard page）。当 `.await()` 需要等待时，它调用 `swapcontext` 切回事件循环——fiber 原地冻结，线程可以跑其他 fiber 或处理 I/O。Promise resolve 时，waker 切回 fiber，精确地从断点继续执行。

这是 libxpp 的主打特性：**C++11，不需要 `co_await` 语法，不需要给函数染色，不需要编译器支持。** 你得到的是和 Rust `.await`、Go goroutine 一样的线性代码体验，用在任何 C++11 工具链上。

### 3. `co_await` / `co_return` —— C++20 协程

```cpp
xpp::Promise<int> compute() {
    int x = co_await fetch_value();
    co_return x * 2;
}
```

如果你有 C++20 编译器，`Promise<T>` 直接就是协程返回类型。`co_await` 编译到同样的 `poll()` / waker 机制上——不需要独立的运行时，不需要 `Task<T>` 包装。

三种风格可以自由混用。`.then()` 链返回的 `Promise<T>` 可以在 fiber 里 `.await()`，协程可以 `co_await` 一个由回调链构建的 Promise，`.then()` 也可以接在协程返回的 Promise 后面。库不关心你选哪种风格——底层都是同样的 `poll()`。

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
- **[Promise 模型](libxpp/promise/)** — poll-waker 状态机，`.await()` 语义（fiber 挂起 + 直接驱动事件循环），C++20 协程帧如何映射到 `Promise<T>`，串联、取消和错误传播的内部机制
- **[异步 I/O](libxpp/io/)** — 从原始 `AsyncFd` 往上经过 `BufReader`/`BufWriter` 到类型安全的 `TcpStream` 和 `File` 的分层架构，以及 `io::copy`、`Duplex`/`Simplex` 等工具
- **[Channel](libxpp/channels/)** — 完整的 Tokio 对齐套件：`oneshot`、`mpsc`（有界用无锁环形缓冲区，无界用无锁链表）、带滞后恢复的 `broadcast`、版本追踪"已读"语义的 `watch`，以及可复用的唤醒原语 `Notify`
- **[线程模型](libxpp/promise/#thread-safety)** — `XPP_MT` 编译开关将 `Shared<T>` 从 `Rc` 切换为 `Arc`，`loom` 模块提供可替换的并发原语用于未来的并发测试，以及所有 channel 的 RAII close 语义
- **[Network](libxpp/net/)** — 异步 TCP、UDP、DNS、TLS，全部建立在同一个 `Promise<T>` 基础上
- **[Filesystem](libxpp/fs.md)** — 异步文件 I/O，带游标追踪，支持 stat、目录操作等
- **[Time](libxpp/time.md)(TODO)** — Tokio 风格的时间原语 — `Instant`、`Duration`、`sleep`、`interval`、`timeout` — 全部基于 `Promise<T>`

贯穿始终的设计哲学是同一个：善用 C++ 已经给我们的东西（移动语义、RAII、协程代码生成），构建一个用起来像 Rust + Tokio 的异步体验，底层跑在 C 的基础上，能融入现有 C++ 项目而无需语言分支或自定义编译器。

---

最终形成的技术栈让 C 和 C++ 各自获得它们应得的异步体验：libx 给需要结构化并发的系统程序员，libxpp 给可以选用 `xpp::fiber([]() { auto v = promise.await(); ... })`（C++11）、`Promise<T>::then([](auto v){...})`（回调链）或 `co_await promise`（C++20 协程）的应用开发者——没有裸指针，没有泄漏的抽象，没有回调金字塔。
