# xpp 异步机制设计 — 技术分享

> 对象：程序员同事 | 时长：约 25 分钟 | 重点：设计决策，非 API 罗列

---

## 0. 开场：C++ 缺一个完整的异步运行时

Go 有 goroutine。Rust 有 tokio。C++ 有什么？

**什么都有，但什么都不统一。**

这是 C++ 生态最尴尬的地方：

```
组件 A：用 libuv 事件循环 + 两个后台线程
组件 B：用 std::thread + 手动线程池
组件 C：用 boost::asio + strand 隔离
组件 D：用 epoll + 自己写的 reactor

→ 四个组件，四种线程模型。你在四套调度系统之间传数据。
→ 业务逻辑永远跟「这个回调在哪个线程跑」纠缠在一起。
→ 线上出了问题，你看一眼 perf 的线程视图就想辞职。
```

**结果就是——**
```c
// 「读一个配置文件，提取第一行」——四层回调：
void on_opened(uv_fs_t *req)         { /* alloc buf, start fstat */ }
void on_stat(uv_fs_t *req)           { /* alloc buf, start read */ }
void on_read(uv_fs_t *req)           { /* 循环 pread, 收齐数据 */ }
void on_done(uv_fs_t *req)           { /* 找到 '\n', 截断, 返回。哪个 buf 忘了 free？*/ }
```

控制流被撕成碎片。每一步是一个孤立回调。你没法 `grep "读完文件后干什么"`——答案散落在四个函数里。

而 Go 的开发者从来不需要想这些——

```go
// Go：同步写法，runtime 搞定一切
data, _ := os.ReadFile("config.txt")
line := strings.SplitN(string(data), "\n", 2)[0]
```

**libxpp 的目标：给 C++ 补上这个缺失的运行时。** 一套统一的 EventLoop。一个类型安全、可组合的异步模型。零线程纠缠在业务代码里。

不是「另一个 C++ 网络库」——是 **C++ 应该长成的样子**。

---

## 1. 统一异步模型：Promise\<T\>

libxpp 的统一语言就一个类型：**`Promise<T>`**——一个「未来的值」。

xpp 重写上面的「读文件取第一行」：

```cpp
// then() 链：函数式风格，每一步都是数据变换
xpp::Promise<String> first_line() {
    return File::open("config.txt")
        .then([](File f) { return f.read_all(); })
        .then([](Vec<uint8_t> bytes) {
            auto text = String::from_utf8(std::move(bytes)).unwrap();
            return text.substr(0, text.find("\n").unwrap_or(text.len()));
        });
}
```

同一个逻辑，也可以用纯 `.await()` 写成一步一步的同步风格：

```cpp
// await 风格：一步一步来，像写 Python
xpp::Promise<String> first_line() {
    auto file  = File::open("config.txt").await();
    auto bytes = file.read_all().await();
    auto text  = String::from_utf8(std::move(bytes)).unwrap();
    return text.substr(0, text.find("\n").unwrap_or(text.len()));
}
```

如果编译器支持 C++20，还能用 `co_await`：

```cpp
// co_await 风格：编译器生成状态机，完全像同步代码
xpp::Promise<String> first_line() {
    auto file  = co_await File::open("config.txt");
    auto bytes = co_await file.read_all();
    auto text  = String::from_utf8(std::move(bytes)).unwrap();
    co_return text.substr(0, text.find("\n").unwrap_or(text.len()));
}
```

三套写法，**底层是同一套 poll/waker 机制**。选哪个看编译器和品味——它们的执行路径完全一样。

**设计要点**：

| 决策 | 为什么 |
|------|--------|
| `then(fn)` 返回 `Promise<U>` | 如果 `fn` 返回 `Promise<U>`，自动 flatten，不会出现 `Promise<Promise<U>>` |
| Move-only | 一个 Promise 只有一个消费者，没有「谁先 await 谁拿值」的语义混乱 |
| 不继承 `std::future` | `std::future` 没有 `then()`，没有组合子，设计哲学完全不同 |

---

## 2. EventLoop — 发动机

在讲 Promise 怎么工作之前，要先讲它跑在什么上面。

```
┌─────────────────────────────┐
│         EventLoop            │
│  ┌───────┐  ┌──────┐        │
│  │ epoll │  │timer │  ...   │   ← 操作系统提供的 I/O 多路复用
│  └───┬───┘  └──┬───┘        │
│      └────┬────┘            │
│           ▼                  │
│      xEventLoopRun()         │   ← 阻塞循环，「有事做才醒」
│                              │
│  WaitScope                   │   ← RAII，绑定线程到 EventLoop
│    enter / leave             │
└─────────────────────────────┘
```

**关键设计**：`EventLoop` 和 `WaitScope` 分离。

- `EventLoop` 只管 create/destroy handle + run/stop
- `WaitScope` 管 enter/leave 线程绑定
- 分离意味着同一个 EventLoop 可以在不同线程 enter（虽然 xpp 目前只支持单线程模式）
- 分离也意味着嵌套 `await()` 安全 —— scope 在栈上，不会交叉污染

---

## 3. 核心循环：poll + waker

这是整个系统的心脏。一句话概括：

> **Promise 是一个状态机，EventLoop 是它的时钟，waker 是它的闹钟。**

```cpp
// Promise<T>::await() 的内部实现（简化）
ValueType await() {
    PromiseWaker waker;
    while (true) {
        // 1. 问：「你好了吗？」
        Option<ValueType> result = m_node->poll(waker);
        if (result.is_some()) {
            return result.unwrap();  // 好了，拿值走人
        }
        // 2. 没好，睡。waker 会在「可能有进展」时叫醒我们。
        waker.park();   // 内部跑一轮 xEventLoopRun(ONCE)
    }
}
```

这就是 **Rust Future 的 poll 模型**，移植到 C++11：

```
poll(waker) → Option<T>
  Some(value)  = Ready        ← 一次性的，拿了就不能再 poll
  None         = Pending      ← waker 被注册，等后续通知
```

**为什么是 one-shot poll 而不是 `is_ready() + take()`？**
- 两步 API 有 TOCTOU（time-of-check-time-of-use）问题
- one-shot 把「检查」和「取走」原子化
- 这也意味着：每个 Promise 只能 await 一次，await 完就空了

---

## 4. PromiseNode 层级 — 六种节点，一种接口

所有 Promise 背后都是 `PromiseNode<T>`，只有一个虚函数：

```cpp
template <class T>
class PromiseNode {
    virtual Option<T> poll(const PromiseWaker &waker) = 0;
};
```

| 节点类型 | 用途 | poll 行为 |
|----------|------|-----------|
| `ImmediatePromiseNode` | 已经是值（`resolve(42)`） | 直接返回 Some，忽略 waker |
| `TransformPromiseNode` | `.then(fn)` | poll 上游 → 拿到值 → 调 fn |
| `ChainPromiseNode` | 自动 flatten | poll 外层 → 拿到 Promise → 切到内层 poll |
| `AdapterPromiseNode` | 外部异步源 | poll 共享的 ResolveState |
| `CoroutinePromiseNode` | `co_await` | poll 协程的 AwaitState |
| `YieldPromiseNode` | yield() | 直接返回 Some(Void{}) |

**这里有一个精妙的设计**：`then()` 链不是 callback 链，是 **节点链**。

```cpp
resolve(10)                    // ImmediatePromiseNode { value: 10 }
  .then([](int x) {            // TransformPromiseNode { dep: ↑, fn: *2 }
      return x * 2;            
  })
  .then([](int x) {            // TransformPromiseNode { dep: ↑, fn: +1 }
      return x + 1;
  });
  // → 结果 = 21

// await() 时做的不是「回调套回调」，而是：
// poll(外)→poll(中)→poll(内)→10→*2→+1→21
```

**好处**：
- 每一步的中间值都不需要堆分配
- 调用栈是浅的（poll 递归展开，不是 callback 嵌套）
- 类型全部静态推导，没有 `std::function` 的虚调用开销

---

## 5. Adapter 模式 — 这是真正「深入」的部分

前面讲的是「链式调用」，但异步编程真正的难点不在这里。真正的难点是：**怎么把外部的异步事件（timer 到期、fd 可读、线程池完成）变成 Promise？**

xpp 的答案是 **Adapter 模式**。

### 问题

假设你有一个 timer。到期时 libx 的 C API 会回调你：

```c
void my_timer_cb(xTimer *t, void *userdata) {
    // timer 到期了！但我怎么通知 await 这个 timer 的代码？
}
```

`my_timer_cb` 和 `await()` 在不同时间、不同调用栈上运行。你需要一座桥。

### 方案：AdapterPromiseNode + PromiseResolver

```
                    ┌──────────────────────┐
                    │  AdapterPromiseNode   │  ← 实现 poll(waker)
                    │  ┌─────────────────┐  │
                    │  │ Arc<ResolveState>│  │  ← 状态共享（Arc）
                    │  │  Option<T> value │  │
                    │  │  AtomicWaker     │  │  ← 线程安全的 waker
                    │  │  atomic<bool>    │  │  ← resolved 标记
                    │  └────────┬────────┘  │
                    │           │            │
                    │  ArcWeak  │  Arc       │
                    │           ▼            │
                    │  ┌─────────────────┐  │
                    │  │ PromiseResolver  │  │  ← 给外部事件持有
                    │  │  .resolve(v)     │  │
                    │  └────────┬────────┘  │
                    └───────────┼────────────┘
                                │
                    ┌───────────▼────────────┐
                    │     TimerAdapter        │  ← 外部事件源
                    │  - 创建 xTimer          │
                    │  - 回调里 resolver      │
                    │    .resolve(value)      │
                    │  - 析构时取消 timer     │
                    └────────────────────────┘
```

### 关键设计决策

**1. Arc + ArcWeak 双引用**

```
AdapterPromiseNode:  Arc<ResolveState>   (强引用，保证 ResolveState 存活)
PromiseResolver:     ArcWeak<ResolveState> (弱引用，Promise 销毁后 resolve 静默丢弃)
```

为什么？因为 `PromiseResolver::resolve()` 可能在任何线程、任何时候调用——包括 Promise 已经 `await()` 完并被销毁之后。用 weak 引用避免了 use-after-free，也不需要 `shared_ptr` + `weak_ptr` 的锁开销。

**2. poll_state() 的三步检查（double-check）**

```cpp
Option<T> poll_state(ResolveState<T> &s, const PromiseWaker &waker) {
    // Step 1: fast path — 已经 resolved 了？
    if (s.resolved.load(acquire)) return std::move(s.value);

    // Step 2: 注册 waker（可能和 resolve 并发）
    s.waker.register_waker(waker);

    // Step 3: double-check — 注册 waker 期间 resolve 了吗？
    if (s.resolved.load(acquire)) {
        s.waker.wake();          // self-wake：我们已经注册了 waker，
        return std::move(s.value); // 但值已经在了，直接拿走
    }

    return none;  // 还没好，去睡
}
```

**为什么需要 double-check？** 看这个并发时序：
1. T1: poll_state 检查 resolved → false
2. T2: resolve() 设 resolved=true → fire waker
3. T1: register_waker
4. T1: 没人再 fire waker 了 → **死等**

加上 double-check 后，T1 在 register_waker 之后再查一次——如果 T2 在此期间 resolved，T1 自唤醒。

**3. 自动取消**

用户代码：

```cpp
auto p = xpp::adapt<int, TimerAdapter>(1000);  // 1 秒后 resolve
p = Promise<int>();  // 销毁 Promise → 触发 ~AdapterPromiseNode → ~TimerAdapter → 取消 timer
```

不需要手动 `cancel()` —— RAII 保证。这是 C++ 的天然优势。

---

## 6. await() 的三种变体

xpp 的 Promise 支持三种使用风格，**底层是同一套 poll 机制**：

### 风格 1：C++11 .await()（直接跑 event loop）

```cpp
int result = fetch_data().await();
// ↓ await() 内部
while (true) {
    if (poll(waker)) return value;
    waker.park();  // → xEventLoopRun(ONCE)
}
```

**适用**：main 线程、顶层入口。

### 风格 2：C++11 + Fiber（栈协程）

```cpp
int result = xpp::fiber([]() {
    int a = fetch_data().await();   // fiber 挂起
    int b = fetch_more().await();   // 继续跑 event loop
    return a + b;
}).await();
```

Fiber 里 `await()` 的行为变了：不阻塞线程，而是 `xFiberYield()` 让出 CPU。

**关键**：Fiber 不需要 `co_await` 语法，不需要 C++20。它在 **await() 函数内部** 判断「我在 fiber 里吗？」——如果在，就 yield；不在，就 run event loop。

### 风格 3：C++20 co_await（原生协程）

```cpp
Promise<int> compute() {
    int a = co_await fetch_data();
    int b = co_await fetch_more();
    co_return a + b;
}
```

C++20 协程把 `compute()` 编译成一个状态机。`co_await` 背后驱动的是同一个 `poll()` 循环。

**设计要点**：`Promise<T>` 本身就是 coroutine return type。不需要额外的 `Task<T>` 类型——`Promise` 直接实现了 `std::coroutine_traits`。

---

## 7. 线程安全：一把锁都没有

这是最容易被忽视但最有意思的部分。

### PromiseResolver 跨线程 resolve

```cpp
// 线程 A: 网络回调
void on_data(const uint8_t *buf, size_t len, void *userdata) {
    auto *resolver = static_cast<PromiseResolver<Vec<uint8_t>>*>(userdata);
    resolver->resolve(Vec<uint8_t>(buf, buf + len));  // ← 线程安全！
}

// 线程 B: 用户代码
Vec<uint8_t> data = read_from_net().await();  // ← 也在 EventLoop 线程
```

**怎么做到**：
- `ResolveState` 用 `std::atomic<bool>` 标记 resolved
- `AtomicPromiseWaker` 用 atomic CAS 保证只有一个 waker 被注册
- `PromiseResolver` 持 `ArcWeak`，Promise 销毁后 resolve 静默丢弃
- `await()` 必须跑在 EventLoop 线程——这是唯一的线程安全约定

**没有 mutex。不需要。** 因为状态转移是单向的（pending → resolved），atomic 足够。

---

## 8. 性能细节

### Per-Chain Arena（链式 arena 分配）

普通的 Promise 链：

```cpp
resolve(1).then(f1).then(f2).then(f3).then(f4)
// 6 个 Node：1 个 Immediate + 4 个 Transform + 1 个 Chain
// 6 次 malloc / 6 次 free
```

xpp 用一个 **256 字节的 bump allocator** 挂在整个链上。所有节点都在这个 arena 里分配：

```
┌─────────────────── 256 bytes ───────────────────┐
│ ImmediateNode │ TransformNode₁ │ TransformNode₂ │ ... │
└─────────────────────────────────────────────────┘
                     ▲ bump pointer
```

- 链上的所有节点共享一个 arena
- 分配：指针往后挪 → 满了 fallback 到 heap
- 释放：整个 arena 一次 free（链结束时）

实际的 malloc 数：**O(1)** 而不是 **O(N)**。

### CompressedPair EBO

```
sizeof(std::vector<int>)  = 24  // ptr + size + cap
sizeof(xpp::Vec<int>)     = 24  // CompactPair<(ptr,size,cap), Allocator>
                                // EBO: GlobalAllocator 是 empty class, 0 字节
                                // 内存布局跟 std::vector 完全一致
```

**安全特性的额外成本：零。**

---

## 9. 与其他异步模型对比

| 模型 | 代表 | 优点 | 缺点 |
|------|------|------|------|
| 回调 | libuv, Node.js | 简单直接 | 控制流碎片化，错误处理痛苦 |
| Future/Promise | xpp, Rust, JS Promise | 链式组合，类型安全 | 需要 `then()` 链或 `await` |
| 协程 | C++20, Go goroutine | 像同步代码 | C++20 要求，编译慢，调试难 |
| Actor | Erlang, Akka | 天然分布 | 消息传递开销，类型边界模糊 |

xpp 的定位：**用 Rust Future 的模型，在 C++11 上跑，保留协程升级路径**。

三条路径的演进关系：

```
      C++11 .then()/.await()
              │
      ┌───────┴────────┐
      ▼                ▼
  C++11 fiber      C++20 co_await
  (栈协程，          (编译器状态机，
   兼容旧代码)        零语法糖开销)
```

全部运行在同一个 EventLoop + poll/waker 核心上。

---

## 10. 总结：C++ 应该这样写

**1. 一个 EventLoop，一个模型，全部组件接入。**
不是「选 libuv 还是 ASIO」——用 libxpp，所有异步操作说的都是同一种语言（`Promise<T>`）。P2P 连接、文件 IO、DNS 解析、定时器——全部跑在一个 EventLoop 上，没有「这个回调在哪个线程」的问题。

**2. Adapter = Arc/ArcWeak + atomic double-check**
零锁的跨线程桥接。构造即启动，析构即取消。不需要生命周期管理 API。任何外部异步源（timer、fd、thread pool）用 10 行 Adapter 就能接入 Promise 系统。

**3. 安全特性零开销**
CompressedPair EBO 让 Vec 和 std::vector 一样大。Arena allocation 让 then 链的 malloc 从 O(N) 降到 O(1)。一切 check 在 debug mode，release 优化掉。

**C++ 不缺库。C++ 缺的是一个不说不同方言的家。** libxpp 就是这个家。

---

## Q&A 预备

**Q: 为什么不直接用 ASIO / libuv？**
A: ASIO 的 `awaitable` 在 C++11 下不可用。libuv 是回调模型，我们需要类型安全的组合。xpp 的 EventLoop 下面嵌的是 libx 自己的事件循环（epoll/kqueue 包装），更轻。

**Q: Fiber 的实现原理？**
A: 用的是 `ucontext` / `makecontext` / `swapcontext`（POSIX 栈协程）。每个 fiber 有自己的栈（默认 128KB）。`xFiberYield()` 把控制权交还给 event loop。

**Q: 为什么不直接用 Rust？**
A: 这是个工程问题不是语言问题。团队 C++ 技术栈，业务代码 C++，依赖库 C++。xpp 的目标是「在 C++ 里写出 Rust 的安全感」，不是「换个语言」。

**Q: 跨平台吗？**
A: POSIX（Linux/macOS）。EventLoop 底层是 epoll（Linux）或 kqueue（macOS）。Windows 暂无支持（IOCP 架构差异大）。

**Q: 既然整个系统是单线程，thread pool 怎么用？**
A: `xpp::work(fn)` 把耗时任务扔到 libx 的后台线程池，返回 `Promise<T>`。resolver 在 worker 线程 resolve，event loop 线程的 await 被 waker 唤醒。两个线程之间没有数据竞争——只有 atomic flag + waker 的并发。
