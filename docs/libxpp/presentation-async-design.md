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
// 「发一个 HTTP GET 请求」——N 层回调：
void on_resolved(uv_getaddrinfo_t *req)  { /* 拿到 IP，socket + connect */ }
void on_connected(uv_connect_t *req)     { /* alloc buf, write request */ }
void on_written(uv_write_t *req)         { /* start read response */ }
void on_data(uv_stream_t *s, ssize_t n)  { /* 拼 buf，解析 status line */ }
void on_more_data(...)                   { /* 解析 headers */ }
void on_body_done(...)                   { /* 找到 body 结束, 回调链终于返回 */ }
```

控制流被撕成碎片。每一步是一个孤立回调。你没法 `grep "发完请求后干什么"`——答案散落在六个函数里。

而 Go 的开发者从来不需要想这些——

```go
// Go：同步写法，runtime 搞定一切
resp, _ := http.Get("https://example.com/api")
defer resp.Body.Close()
body, _ := io.ReadAll(resp.Body)
fmt.Println(string(body))
```

**libxpp 的目标：给 C++ 补上这个缺失的运行时。** 一套统一的 EventLoop。一个类型安全、可组合的异步模型。零线程纠缠在业务代码里。

不是「另一个 C++ 网络库」——是 **C++ Done Right**。

---

## 1. 统一异步模型：Promise<T>

**一次异步调用，本质上只有一件事**：

> 被调方交出一个 `Promise<T>`，意思是「值以后会到，你拿着这个等我」。

调用方拿到 Promise 后，三个选择消费它：

| 消费方式          | 含义                | 语法                       |
| --------------- | ----------------- | ------------------------ |
| `.await()`      | 我现在就要等             | C++11，显式 poll 循环        |
| `.then(fn)`     | 我不等，值到了帮我调 `fn`   | C++11，链式组合              |
| `co_await`      | 编译器帮我写状态机，写法等同同步代码 | C++20                    |

三种写法底层跑同一套 `poll/waker` 机制——选哪个看编译器和品味，执行路径完全一样。

下面用「发 HTTP GET 请求，读取 body」示范这三种写法。同一逻辑，三种表达。

```cpp
// 风格 1：then() 链——函数式风格，每一步都是数据变换
xpp::Promise<Vec<uint8_t>> fetch_body(const char *url) {
    return xpp::http::get(url)
        .then([](xpp::http::Response resp) {
            // resp.body() 返回一个 reader（满足 AsyncReader duck type）
            // xpp::io::read_all 是模板，对任何带 read() 的 reader 都通用
            return xpp::io::read_all(resp.body());
        });
}
```

```cpp
// 风格 2：await 风格——一步一步来，像写 Python
xpp::Promise<Vec<uint8_t>> fetch_body(const char *url) {
    auto resp = xpp::http::get(url).await();
    return xpp::io::read_all(resp.body());
}
```

```cpp
// 风格 3：co_await 风格——编译器生成状态机，完全像同步代码
xpp::Promise<Vec<uint8_t>> fetch_body(const char *url) {
    auto resp = co_await xpp::http::get(url);
    co_return co_await xpp::io::read_all(resp.body());
}
```

> **为什么不绕开示例**：HTTP GET 这个例子用到了两层异步——`http::get` 跨网络拿 Response，`io::read_all` 循环读流。两层拼接用三种写法各自怎么写，就是接下来要展示的；真正的端到端流程（DNS / connect / read 响应）留给 Section 3。

**设计要点**：

| 决策                         | 为什么                                                           |
| -------------------------- | ------------------------------------------------------------- |
| `then(fn)` 返回 `Promise<U>` | 如果 `fn` 返回 `Promise<U>`，自动 flatten，不会出现 `Promise<Promise<U>>` |
| Move-only                  | 一个 Promise 只有一个消费者，没有「谁先 await 谁拿值」的语义混乱                      |
| 不继承 `std::future`          | `std::future` 没有 `then()`，没有组合子，设计哲学完全不同                      |

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

### 一次完整调用，端到端走读

用 `fetch_http()` 的 `await` 版本，把每个步骤拆开看。

```cpp
// 一次 HTTP GET，返回 Response
xpp::Promise<xpp::io::Result<xpp::http::Response>> fetch_http(const char *url) {
    return xpp::http::get(url).await();
}
```

`http::get` 内部要做三件事：DNS 解析、TCP connect、HTTP 请求/响应读写。我们把 connect 完成后那一阶段（已经拿到 connected socket fd）放大看，poll/waker 在哪里触发。

```
fetch_http() 被调用：
│
├─ ① http::get(url)
│     → 内部拿到 connected socket fd
│     → fd 包装成 AsyncFd：fcntl 设为 non-blocking
│     → 通过 xEventAdd 注册到 EventLoop（edge-triggered，Read|Write）
│     → 返回 Promise<Response>，背后是 AdapterPromiseNode
│
├─ ② co_await / .await()
│     → 进入 poll 循环（就是下面那个 while(true) 循环）
│     │
│     ├─ 第 1 轮 poll:
│     │    AdapterNode poll：「response 数据就绪了吗？」
│     │    底层 AsyncFd 检查内部 readiness → false（还没收齐响应）
│     │    回答：Pending
│     │    副作用：waker 被注册到底层等待表里
│     │            （EventLoop 不直接知道 waker——它只管 fd→回调 的映射）
│     │    → 代码走到 waker.park()
│     │
│     ├─ waker.park()
│     │    → 调用 xEventLoopRun(ONCE)
│     │    → 最终调用 epoll_wait(timeout, ...)
│     │    → 当前线程阻塞，等内核通知
│     │
│     ├─ 内核：服务器响应到达，socket fd 变为可读
│     │    → epoll 返回这个 fd 的事件
│     │    → EventLoop 调用 AsyncFd 注册的回调（fd→回调 映射在 EventLoop 里）
│     │    → 回调里 resolve() → wake waker（查等待表，触发注册过的 waker）
│     │    → park() 返回，线程醒来
│     │
│     ├─ 第 2 轮 poll:
│     │    AdapterNode 再次 poll：「response 数据就绪了吗？」
│     │    AsyncFd 检查 → true（epoll 刚告诉我们 fd 可读了）
│     │    回答：Ready(Response 对象)
│     │    → await 拿到 Response
│     │
│     └─ 一次 await 完成。实际 HTTP 场景往往多轮 poll：
│        read 一段 → 还没收齐 EAGAIN → park → epoll → 再 read →
│        ... 直到 status line + headers + body 全部解析完才 Ready。
```

**每一层 `co_await` 都在重复同一件事**：poll → 没好就 park → EventLoop 跑一轮 → 被内核叫醒 → poll → 拿到值。整个循环的伪代码就下面这几行。

### 引擎视角：await() 内部循环

```cpp
// Promise<T>::await() 的内部实现（简化）
ValueType await() {
    PromiseContext cx;
    while (true) {
        // 1. 问：「你好了吗？」
        Option<ValueType> result = m_node->poll(cx);
        if (result.is_some()) {
            return result.unwrap();  // 好了，拿值走人
        }
        // 2. 没好，睡。cx 会在「可能有进展」时叫醒我们。
        cx.park();   // 内部跑一轮 xEventLoopRun(ONCE)
    }
}
```

这就是 **Rust Future 的 poll 模型**，移植到 C++11：

```
poll(cx) → Option<T>
  Some(value)  = Ready        ← 一次性的，拿了就不能再 poll
  None         = Pending      ← cx.waker() 被注册，等后续通知
```

**为什么是 one-shot poll 而不是 `is_ready() + take()`？**

- 两步 API 有 TOCTOU（time-of-check-time-of-use）问题
- one-shot 把「检查」和「取走」原子化
- 这也意味着：每个 Promise 只能 await 一次，await 完就空了

> **注**：本节只讲流程层面——poll/waker 在哪里触发、park 怎么被唤醒。AsyncFd 内部用什么数据结构把「waker 注册到等待表」做对、跨线程安全性怎么保证，是 Section 5「Adapter 模式」的话题。Section 3 的读者只需要知道：fd 注册到 EventLoop、回调里能 resolve、waker 被 wake，就够了。

---

## 4. 双人舞：PromiseNode 与 PromiseResolver

Promise 系统由**五个部件**组成。理解它们的职责和关系，就理解了全部：

```
用户代码
  │
  ▼
Promise<T>  ←─────────────────────────────────────────────────────┐
  │  句柄。用户只跟它交互                                           │
  │  .then(fn) / .await() / co_await                              │
  ▼                                                               │
PromiseNode<T>  ←── 问端                                           │
  │  虚基类。poll(waker) → Option<T>                               │
  │  六种子类：Immediate / Transform / Chain / Adapter /           │
  │  Coroutine / Yield                                             │
  │                                                               │
  │  所有 node 从 Arena 分配 — 跟 malloc 无关                       │
  │  then() 链的 node 在同一个 arena 里连着分配，一次 free 全部释放   │
  │                                                               │
  ▼                                                               │
ResolveState<T>  ←── 数据的家                                       │
  │  Option<T> value         ← 值住在这里                           │
  │  AtomicWaker waker       ← 记录谁在等                           │
  │  atomic<bool> resolved   ← 「好了没」                            │
  │                                                               │
  │  PromiseNode    持有 Arc<ResolveState>    — 强引用               │
  │  PromiseResolver 持有 ArcWeak<ResolveState> — 弱引用              │
  └─────────┬──────────────────────────────────────────────────────┘
            │
   ┌────────┴────────┐
   ▼                 ▼
PromiseResolver<T>  PromiseContext
  答端                 闹钟
  .resolve(value)      .park() / .waker().wake()
  生产者调用            驱动 poll 循环
```

**PromiseContext**：连接 PromiseNode 和 EventLoop 的中间人。`poll()` 返回 None 时，`cx.waker()` 被注册到 `ResolveState` 的 `AtomicWaker` 里；外部事件调 `resolve()` 时，waker 被 wake，`park()` 返回，再来一轮 poll。

**Arena**：PromiseNode 不从 heap malloc。每个 PromiseNode 自带一个 arena chunk，bump pointer 分配——`then()` 链上的所有中间 node 在同一个 arena 里接连分出来，全部释放时一次 free。O(N) 的 malloc 降为 O(1)。

---

### 4.1 PromiseNode — 问端：六种节点，一个接口

所有 Promise 背后都是 `PromiseNode<T>`，只有一个虚函数：

```cpp
template <class T>
class PromiseNode {
    virtual Option<T> poll(const PromiseContext &cx) = 0;
};
```

六种节点的区别在&#x4E8E;**「值从哪来」**。按来源分三组：

| 来源                | 节点                     | 什么时候 poll 返回 Some                                        | 示例                                                  |
| ----------------- | ---------------------- | -------------------------------------------------------- | --------------------------------------------------- |
| **已有值**           | `ImmediatePromiseNode` | 马上。`resolve(42)` — 值早就在了，不需要等                            | `xpp::resolve(42)` / `xpp::resolve()`               |
|                   | `YieldPromiseNode`     | 马上。`yield()` — 专门给事件循环留个呼吸口                              | `xpp::yield()`（`defer(fn)` 内部就是它套一层 then）           |
| **从别的 Promise 来** | `TransformPromiseNode` | poll 上游拿到值 → `fn(value)` ⚠️ 如果 fn 返回 Promise，自动委托给 Chain | `p.then([](int x){ return x * 2; })`                |
|                   | `ChainPromiseNode`     | poll 上游拿到 Promise → 切到内层继续 poll                          | `p.then([]{ return inner; })` — fn 返回 Promise 时自动生成 |
| **从外部世界来**        | `AdapterPromiseNode`   | poll 共享的 `ResolveState` — 外部事件调 `resolve()` 时值到位         | `after(100)` / `work([]{ ... })`                    |
|                   | `CoroutinePromiseNode` | poll `co_await` 的 `AwaitState` — 协程自己管理挂起/恢复             | 协程体里的 `co_await some_promise;`                      |

**关键洞察**：`then()` 链不是 callback 链，是 **节点链**。

```cpp
resolve(10)                    // ImmediatePromiseNode { value: 10 }
  .then([](int x) {            // TransformPromiseNode { dep: ↑, fn: *2 }
      return x * 2;            
  })
  .then([](int x) {            // TransformPromiseNode { dep: ↑, fn: +1 }
      return x + 1;
  });
  // → 结果 = 21

// await() 内部做的事不是「回调套回调」：
// poll(外) → poll(中) → poll(内) → 10 → *2 → +1 → 21
```

**为什么节点比回调好**：

- 每一步的中间值都不需要堆分配 — 值沿着链传递，不拷贝
- 调用栈是浅的 — poll 递归展开，不是 callback 嵌套
- 类型全部静态推导 — 没有 `std::function` 的虚调用开销

---

### 4.2 PromiseResolver — 答端：「问」和「答」为什么必须分家

PromiseNode 只管「问」，但谁来「答」？**PromiseResolver**。

这不是一个可有可无的辅助类 —— 而是 Promise 系统的**另一半**。异步编程里「问」和「答」天然在不同地方：

- **问端**：用户写 `read_all().then(decode).await()` — 在 EventLoop 线程里
- **答端**：fd 可读了、timer 到期了、thread pool 完成了 — 在一些外部回调里

Promise 的设计把两端拆成两个独立类型，各持自己的引用，中间通过 `ResolveState` 共享状态：

```
PromiseNode 持有 Arc<ResolveState>        — 强引用。「只要我还活着，state 就在」

PromiseResolver 持有 ArcWeak<ResolveState> — 弱引用。「如果 Promise 已经销毁了，
                                             我的 resolve() 就静默丢弃，不 crash」
```

**为什么是 ArcWeak？** `PromiseResolver::resolve()` 可能在任何线程、任何时候调用。如果 Promise 已经 `await()` 完并且被析构了，`ResolveState` 已经释放。Weak 引用让 Resolver 安全地检测到「没人等了」而不发生 use-after-free。

```cpp
// 创建一个 Promise/Resolver 对
auto [promise, resolver] = xpp::async<int>();

// 消费者（问端）
promise.await();              // 在 poll 循环里等

// 生产者（答端 — 可以在任何地方、任何时候调用）
resolver.resolve(42);         // 「好了，给你值」
```

---

### 4.3 两种答端模式

PromiseResolver 的连接方式有两类，区别在于**谁拿着 Resolver**：

| 模式                  | 谁 resolve | 典型场景                        | Resolver 生命周期   |
| ------------------- | --------- | --------------------------- | --------------- |
| **直接 resolve**      | 你手上的代码    | `.then()` 里异步调用 `resolve()` | 你手动管理           |
| **Adapter resolve** | 外部事件源回调   | timer 到期回调「顺便」调 `resolve()` | Adapter 析构时自动取消 |

第二种是 Adapter 模式的核心——也是下一节的内容。

**两种模式共享同一个 `poll_state()` 算法**（下一节会看到为什么这个算法的 double-check 设计是必须的）。

---

## 5. Adapter 模式 — 这是真正「深入」的部分

前面讲的是「链式调用」，但异步编程真正的难点不在这里。真正的难点是：**怎么把外部的异步事件（timer 到期、fd 可读、线程池完成）变成 Promise？**

xpp 的答案是 **Adapter 模式**。

### 问题

假设你有一个 timer。到期时外部 timer 库会回调你（示意，非 xpp 实际 API）：

```c
void my_timer_cb(xTimer *t, void *userdata) {
    // timer 到期了！但我怎么通知 await 这个 timer 的代码？
}
```

`my_timer_cb` 和 `await()` 在不同时间、不同调用栈上运行。你需要一座桥。

### 方案：AdapterPromiseNode + PromiseResolver

```
                    ┌──────────────────────┐
                    │  AdapterPromiseNode   │  ← 实现 poll(cx)
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

为什么？因为 `PromiseResolver::resolve()` 可能在任何线程、任何时候调用——包括 Promise 已经 `await()` 完并被销毁之后。用 weak 引用避免了 use-after-free。

**2. poll_state() 的三步检查（double-check）**

```cpp
Option<T> poll_state(ResolveState<T> &s, const PromiseContext &cx) {
    // Step 1: fast path — 已经 resolved 了？
    if (s.resolved.load(acquire)) return std::move(s.value);

    // Step 2: 注册 waker（可能和 resolve 并发）
    s.waker.register_by_ref(cx.waker());

    // Step 3: double-check — 注册 waker 期间 resolve 了吗？
    if (s.resolved.load(acquire)) {
        cx.waker().wake();    // self-wake：我们已经注册了 waker，
        return std::move(s.value); // 但值已经在了，直接拿走
    }

    return none;  // 还没好，去睡
}
```

**为什么需要 double-check？** 看这个并发时序：

1. T1: poll_state 检查 resolved → false
2. T2: resolve() 设 resolved=true → wake waker
3. T1: register_by_ref
4. T1: 没人再 wake waker 了 → **死等**

加上 double-check 后，T1 在 register_by_ref 之后再查一次——如果 T2 在此期间 resolved，T1 自唤醒。

**3. 自动取消**

用户代码：

```cpp
{
    auto p = xpp::adapt<int, TimerAdapter>(1000);  // 1 秒后 resolve
    // ... 用 p 干点事
}  // ← p 析构 → ~AdapterPromiseNode → ~TimerAdapter → 取消 timer
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
    if (poll(cx)) return value;
    cx.park();  // → xEventLoopRun(ONCE)
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

## 7. 调用拓扑 — 谁调谁，怎么传值

前 6 节讲的是单点机制：一个 Promise 怎么 poll、怎么 resolve。但真实代码里，调用方和被调方分布在不同位置——同线程、跨线程、同 fiber、跨 fiber、走 channel、不走 channel。这一节把这些拓扑梳理清楚。

### 7.1 三种值传递路径

一个 Promise 被消费完拿到值，有三种底层路径，对应前 6 节的三类机制：

| 路径           | 机制                                       | 谁来 resolve              | 典型场景                    |
| ------------ | ---------------------------------------- | ---------------------- | ----------------------- |
| **直接调用**      | 被调方返回一个 Promise，调用方 `.await()`/`.then()`   | 被调方自己（在 EventLoop 回调里） | `File::open` / timer    |
| **Channel**  | 被调方是另一个持有 `Sender` 的代码块，调用方持有 `Receiver`  | Sender 的 `send()`      | 多任务协作、流式数据              |
| **后台 Worker** | 被调方是后台线程池里的函数                           | Worker 线程跑完后调 `resolve` | CPU 密集任务                |

后两种是第一种的「被调方不在调用方的 EventLoop 同步可达范围」时的扩展。

### 7.2 Channel — 并发协调

Promise 管「做完一件事通知我」，channel 管「多个事之间怎么传数据」——两套东西正交互补，都跑在 EventLoop 上。

sync 模块提供 Tokio 风格的 channel：

- `oneshot` — 单发单收
- `mpsc` — 多生产者单消费者
- `broadcast` — 一对多
- `watch` — 值版本跟踪

```cpp
// 工厂：bounded mpsc，cap=64
auto [tx, rx] = xpp::sync::mpsc::channel<int>(64);

// 多个生产者：copy 出多份 Sender，各自 send
auto tx2 = tx;
tx.send(1).await();               // 缓冲满则挂起协程
tx.send(2).await();
tx2.send(100).await();            // 跟 tx 并发，互不阻塞

// 单一消费者：None 表示所有 sender 都 drop 了、channel 自动 close
while (auto v = rx.recv().await()) {
    use(v.unwrap());
}
```

`send()` 和 `recv()` 内部都是 AdapterPromiseNode——channel 的本质就是「把一次 send/recv 配对包装成 Promise」。所以前 6 节的所有机制（poll/waker/arena）对 channel 全部适用。

### 7.3 Work — CPU 密集任务的后台线程池

单个 EventLoop 是单线程的，CPU 密集任务不能堵住它。`xpp::work(fn)` 把任务扔到 libx 的后台线程池：

```cpp
// 主线程：发起一个 CPU 密集的计算，不阻塞 event loop
auto result = xpp::work([]() -> int {
    // 这段在后台线程跑
    int sum = 0;
    for (int i = 0; i < 1000000; i++) sum += i;
    return sum;
}).await();

// result 拿到时，主线程从未被阻塞过——
// await() 内部的循环在等 worker 线程 resolve 时才 park，
// 其他时间 event loop 照常处理 fd 事件和 timer。
```

机制：`work(fn)` 返回一个 Promise（背后是 AdapterPromiseNode），把 `fn` 提交到后台线程池；worker 线程跑 `fn`，跑完调 `resolver.resolve(value)`；EventLoop 线程的 await 被 waker 唤醒。两个线程之间没有数据竞争——只有 atomic flag + waker 的并发。

实际场景：读文件 + 解析 + 写回，解析在 worker 线程

```cpp
auto data = File::open("data.bin").await().read_all().await();
auto parsed = xpp::work([&]() {
    return heavy_parse(data);  // CPU 密集，扔到线程池
}).await();
File::open("output.bin").await().write_all(parsed.as_bytes()).await();
```

### 7.4 多 EventLoop 拓扑

EventLoop 单线程，**不意味着整个软件只能跑一个线程**。可以开多个 EventLoop 各跑各的线程，再靠 channel 互通。

```
┌──────────────┐         ┌──────────────┐
│  EventLoop A  │  mpsc   │  EventLoop B  │
│  (thread 1)   │ ──────> │  (thread 2)   │
│  fiber/await  │  channel│  fiber/await  │
└──────────────┘         └──────────────┘
```

A 线程的 fiber 里 `tx.send(v).await()`，B 线程的 fiber 里 `rx.recv().await()` 拿到——channel 跨线程安全，waker 跨 EventLoop 唤醒。

### 7.5 六种调用场景

把「同/跨线程 × 同/跨 fiber × 直接/channel」三个维度组合，穷举所有调用拓扑：

| # | 线程 | fiber | 通信方式   | 典型机制                                     |
| - | ---- | ----- | ------ | ---------------------------------------- |
| 1 | 同    | 同     | 直接     | `then` 链 / `co_await` 链 — 被调方返回 Promise，本 fiber 消费 |
| 2 | 同    | 同     | channel | 同 fiber 里 `tx.send().await()` + `rx.recv().await()` |
| 3 | 同    | 跨     | 直接     | fiber A `await()`，fiber B 在某处 `resolver.resolve()` |
| 4 | 同    | 跨     | channel | 同 EventLoop 上多 fiber 用 mpsc/oneshot 通信     |
| 5 | 跨    | 跨     | 直接     | `xpp::work(fn)` — worker 线程池跑完直接 resolve    |
| 6 | 跨    | 跨     | channel | 多个 EventLoop 各跑线程，channel 互通               |

**一个边界情况**：Adapter 模式（timer 到期 / fd 可读）的 `resolve` 来自 EventLoop 调度回调，不在任何 fiber 里。按这个分类维度，它落进 #1——await 在某 fiber 内，resolve 来自同线程的 EventLoop 回调，没有跨 fiber 也没有跨线程。严格说"被调方"不是 fiber 而是 EventLoop 本身，但值传递路径等价于 #1。

---

## 8. 性能细节

### Per-Chain Arena（链式 arena 分配）

普通的 Promise 链：

```cpp
resolve(1).then(f1).then(f2).then(f3).then(f4)
// 1 个 Immediate + 4 个 Transform
// （fn 返回普通值时不生成 Chain；只有 fn 返回 Promise 时才额外追加 ChainNode）
// 不含 arena：6 次 malloc / 6 次 free
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

---

## 9. 与其他异步模型对比

| 模型             | 代表                                | 优点                 | 缺点                          |
| -------------- | --------------------------------- | ------------------ | --------------------------- |
| 回调             | libuv, Node.js                    | 简单直接               | 控制流碎片化，错误处理痛苦               |
| 无栈协程 (Future/Promise) | Rust Future, C++20 co_await, xpp, JS Promise | 链式组合，类型安全，零栈开销      | 需要显式 `await` / `then` / `co_await` |
| 有栈协程           | Go goroutine, xpp fiber, ucontext          | 同步写法，旧 C++ 也能用      | 每协程独立栈（KB~MB），调度不可控          |
| Actor          | Erlang, Akka                      | 天然分布               | 消息传递开销，类型边界模糊               |

xpp 的定位：**用 Rust Future 的模型，在 C++11 上跑，保留协程升级路径**。

两种机制路径共享同一个 EventLoop + poll/waker 核心：

```
              EventLoop + poll/waker
                       │
            ┌──────────┴──────────┐
            ▼                     ▼
        无栈协程                  有栈协程
   ┌─────┴─────┐                  │
   ▼           ▼                  ▼
C++11 .await()  C++20 co_await  C++11 fiber
(显式 poll/waker,  (编译器生成状态机,  (makecontext 启动,
  C++11 兼容)      零语法糖开销)      _setjmp/_longjmp 切换, 旧代码可用)
```

---

## 10. 总结：C++ Done Right

**1. Poll + Waker — 跟 Rust Future 同构的异步核心**  
一个接口 `poll(waker) → Option<T>` 驱动全部。Readiness check 和 value 取走原子化，避免了 TOCTOU。waker 连接 EventLoop，外部事件触发时自唤醒。

**2. PromiseNode — 六种节点，值从哪来**  
已有值（Immediate / Yield）、从上游 Promise 来（Transform / Chain）、从外部世界来（Adapter / Coroutine）——三类来源，职责清晰。`.then()` 链就是节点链，不是回调链。

**3. PromiseNode + PromiseResolver — 问答分离**  
PromiseNode 只问不管答。答端 PromiseResolver 用 ArcWeak 弱引用安全地跨线程 resolve——强引用在、就投递值；已析构、就静默丢弃。

**4. Adapter 模式 — 任意外部源接入**  
Arc/ArcWeak + poll_state() atomic double-check。构造即启动，析构即取消。timer、fd、thread pool 用 10 行 Adapter 就能接入 Promise 系统。

**5. 多种消费风格，一个内核**  
C++11 `.await()`、C++20 `co_await`、C++11 fiber——三套写法，底层是同一套 poll/waker。无栈有栈并行可选，不互相排斥。

**6. 零开销**  
Arena allocation 让 then 链的 malloc 从 O(N) 降到 O(1)。所有 debug check 在 release 优化掉。

**C++ 不缺库。C++ 缺的是一套不说不同方言的异步语言。** libxpp 给出的就是这门语言。

---

## 11. 为什么 Rust 的设计能搬到 C++

> Rust 和 C++ 在值语义层高度同构。安全层不同构——搬不过来的得用设计补。

### 值语义层——直译

Rust 的类型系统本来长在 C++ 的根上。这些是**直译**，不是仿写：

| Rust                | C++ (xpp)               | 为什么是同一回事                            |
| ------------------- | ----------------------- | ----------------------------------- |
| `Option<T>`         | tagged union + bool     | 都是 sum type：值是 T，或者不是               |
| `Result<T, E>`      | 同上                      | 同一个代数——要么有值，要么有错误                   |
| move 默认，Copy opt-in | move ctor + `std::move` | move semantics 是 C++11 发明的，Rust 学去的 |
| `impl Drop`         | `~T()`                  | RAII 是 C++ 发明的，Rust 拿走了             |
| `Future::poll`      | `PromiseNode::poll`     | 同一个问题（「好了没」），同一个答案（poll + waker）    |
| 泛型单态化               | 模板实例化                   | 两个编译器做一模一样的代码膨胀                     |

**RAII、move、泛型——这三根柱子就是 C++ 给的。** Rust 只是扫干净了 unsafe 的后路，没发明新柱子。

### 安全层——重写

Rust 的类型系统会**在编译期证明**你的代码没有数据竞争、没有悬垂引用、所有 enum 分支都处理了——证明不出来编译就过不了。C++ 编译器不做这种证明，它只检查语法和基本类型——内存安全靠程序员自觉。这些**搬不过来**：

| Rust 证明        | C++ 没有          | xpp 的对策                                                |
| -------------- | --------------- | ------------------------------------------------------ |
| borrow checker | 引用就是裸指针         | 单线程 EventLoop + `Arc/ArcWeak` — 靠架构规避多所有者              |
| `Send + Sync`  | 没有              | `poll_state()` 的 acquire/release + atomic double-check |
| `match` 穷尽性    | `if/else` 漏了不报错 | `Option<T>` + debug assert — 漏了至少炸，不会悄无声息              |
| lifetime 注解    | 注释，靠人读          | Move-only Promise — 消费即销毁，没有持有引用的问题                    |

**核心差异**：Rust 的安全是证明出来的（编译器算），C++ 的安全是约定出来的（你不犯错就没事）。xpp 的设计是在约定之上加了一层检查——debug assert 让犯错变成 crash，而不是 silent corruption。

但 libxpp 充分利用了「值语义」让使用者尽可能的避免了这些问题——比如 `Promise<T>` 是 move-only 的，await 完一次就空了，**不存在两个地方同时持有同一个未完成的 Promise**；`File` 析构即关闭 fd，`Vec<T>` 移动后原对象变空——**所有权在类型层面就写死了**。

### 一句话

> **xpp 不是「用 C++ 写 Rust」。xpp 是用 C++ 自己的工具（RAII、move、模板、atomic）重新实现 Rust API 背后那个「用类型说话」的哲学。**

---

## Q\&A 预备

**Q: 为什么不直接用 ASIO / libuv？**  
A: 定位不同。ASIO / libuv 是**事件库**——给 epoll/kqueue 套层回调基础设施。xpp 想要的是**统一且完备的异步运行时**：同一套 `Promise<T>` 模型 + EventLoop 之上，封装 IO / Net / HTTP / Channel 等基础模块，让业务代码用同一种异步语言写所有事情。

- **统一** — 从 `File::open` 到 `TcpStream::connect` 到 `mpsc::send`，全部返回 `Promise<T>`，不需要把 ASIO 的回调再手动包成 Promise。
- **完备** — 运行时自带文件 IO、TCP/UDP/DNS、HTTP、Channel、Fiber 等模块，不是只有事件骨架让用户自己造轮子。

直接用 ASIO / libuv 意味着要么在它们的回调模型上重造 Promise（ASIO `awaitable` C++11 不可用，libuv 是纯回调），要么接受「基础模块用 ASIO、async 用 xpp」这种割裂的栈。

**Q: 那为什么不基于 ASIO / libuv 做？**  
A: 四个原因。

- **范式冲突** — ASIO / libuv 是**推模型**（completion handler:操作完成 → ASIO 主动调你的回调），xpp 是**拉模型**（poll/waker:你自己问「好了没」，没好就睡等 waker 叫醒）。要在推模型上嫁接拉模型，每个 ASIO 异步操作都要插入「推转拉」中间层：completion handler → resolver.resolve() → waker.fire()，每一步都是一次 indirect call + atomic CAS。更致命的是两个 run loop（ASIO 的 `io_context`、xpp 的 `EventLoop`）必须一个嵌在另一个里面跑——谁的 IO 超时谁说了算？谁先调度谁？这类耦合在设计层就没干净答案。

- **结构设计** — libx 的事件循环通过 `xEventLoopEnter` / `xEventLoopLeave` 绑定到线程（`__thread` TLS），任何回调里直接用 `xEventLoopCurrent()` 拿当前 loop。ASIO / libuv 相反——`io_context` / `uv_loop_t*` 必须在每个 API 调用和回调里手传，一旦传错线程就是数据竞争甚至死锁。前者声明式：线程拥有 loop；后者命令式：你在哪里跑 loop 是你自己的事——代价就是把所有风险摊给调用者。

- **性能** — `event_bench.cpp` 里的 libuv baseline 对比压测，libx 的事件循环在 wake latency、timer、offload 等微指标上吊打 libuv。ASIO 同理。自己写的事件循环在这件事上没输过。

- **内存模型** — Timer/Work 有对象池（freelist，最多缓存 256 个 timer struct），跨线程通信用 lock-free MPSC + Treiber stack，wake 带 coalescing（重复调用跳过 syscall）。libuv 到处 malloc 没有池，ASIO 内部用 mutex 护队列——高频小对象分配的开销不在一个数量级。

- **Taste** — ASIO / Boost 的设计臃肿，命名风格跟 STL 一样诡异：`std::vector` 是类型,`std::move` 是函数——同一种 `lowercase_underscored` 风格,类型和值不分。读一行代码需要额外的脑力判断「这是类还是函数」。我宁可从 OS API 直写，简洁、干净、自己说了算。

**Q: 为什么不直接用 Rust？**  
A: 这是个工程问题不是语言问题。团队 C++ 技术栈，业务代码 C++，依赖库 C++。xpp 的目标是「在 C++ 里写出 Rust 的安全感」，不是「换个语言」。

**Q: 跨平台吗？**  
A: Linux 走 epoll，macOS 走 kqueue，Windows 走 WSAPoll——三种后端通过同一个接口提供 edge-triggered 事件通知（WSAPoll 底层是 poll，通过禁用事件 + re-arm 模拟 ET）。后续计划做第二套 proactor 事件机制：IOCP 支持 Windows，io_uring 支持 Linux/macOS。

**Q: Fiber 的实现原理？**  
A: 每个 fiber 有自己的栈（mmap 分配，默认 128KB）。首次切换用 `makecontext`/`setcontext` 设置新栈和入口函数；后续挂起/恢复全走 `_setjmp`/`_longjmp`，避开 ucontext 的 sigprocmask 开销。`xFiberYield()` 把控制权交还给 event loop。

---

## 12. Appendix: Beyond Async — The Rest of libxpp

Promise 是骨架。但同构移植不只异步机制——libxpp 还搬了 Rust 的整个标准库哲学：用类型系统取代文档和注释。

下面逐个看「之前 C++ 怎么写 → libxpp 怎么写 → 为什么」。

### Option<T> — A Value or Nothing

**C++ 现状**：用 `nullptr`、`std::optional`（C++17 起）、或 `T*` + 人肉检查。

```cpp
// C++ 典型写法：用指针表"可能没有"
T* find_user(const std::string& email) {
    // ...
    return nullptr;  // 表示没找到。调用者必须记住检查——编译器不帮你。
}

// 调用方：
T* user = find_user("bob@x.com");
use(user->name);  // ← 忘了检查？UB。crash 或更糟——静默错误。
```

**libxpp 写法**：

```cpp
Option<T> find_user(const String& email) {
    // ...
    return none;  // 类型系统承载语义——返回类型就告诉调用者：可能为空
}

// 调用方被迫面对「可能没有」的事实：
auto user = find_user("bob@x.com");
if (user.is_some()) {
    use(user.unwrap().name);  // 安全——unwrap() 在 debug 模式 assertion 检查
}
```

**设计理由**：`is_some()`/`is_none()` 显式，不依赖隐式 `operator bool()`。`unwrap()` debug 模式下检查，release 优化掉。

### Result<T, E> — Success or Error

**C++ 现状**：用返回值码（`int`）、`errno`、异常三者混用，每个库有自己的错误处理。

```cpp
// C++ 典型写法：int 返回值码 + 通过 out-param 传真正结果
int parse_config(const char* path, Config* out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;      // 错误返回 errno
    
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0) return -errno;        // 另一个错误路径
    
    if (!validate(buf, n)) return -1; // -1 是什么意思？文档在哪？
    *out = decode(buf, n);            // 成功通过 out-param 传出
    return 0;                         // 0 = 成功，惯例而已
}

// 调用方必须检查返回值——忘了就是 bug：
Config cfg;
parse_config("app.cfg", &cfg);  // 忘了检查返回值！crash 在别处，难查。
```

**libxpp 写法**：

```cpp
Result<Config, Error> parse_config(const char* path) {
    auto file = XPP_TRY(File::open(path));   // XPP_TRY 宏：失败直接 return err(...)
    auto data = XPP_TRY(file.read_all());
    auto cfg  = XPP_TRY(decode(data));         // 类型系统保证：要么是 Config，要么是 Error
    return cfg;
}

// 调用方没法「忘了处理错误」——类型签名里就写着 Result：
auto result = parse_config("app.cfg");
if (result.is_ok()) {
    use(result.unwrap());          // 安全
} else {
    log("failed: {}", result.unwrap_err());
}
```

**设计理由**：不抛异常（项目无 RTTI、无异常），不用 out-param，不用 `goto cleanup`。`XPP_TRY` 宏让错误传播变成一行，对标 Rust 的 `?`。

### Vec<T, Alloc> — Contiguous Growable Array

**C++ 现状**：`std::vector` 很好，但 OOM 时抛 `std::bad_alloc`——项目不用异常。

**libxpp 写法**：

```cpp
// 双 API：便利版和显式版
Vec<int> v;
v.push(42);                    // OOM 时 XPP_ASSERT（debug 炸，release 信任调用者）
auto r = v.try_push(42);       // 返回 Result<void, AllocError>，显式处理
if (r.is_err()) { handle_oom(); }
```

**设计理由**：

| std::vector             | xpp::Vec                                  |
| ----------------------- | ----------------------------------------- |
| OOM → throw `bad_alloc` | OOM → `Result<void, AllocError>` 或 assert |
| `front()` 空容器 UB        | `first()` → `Option<T&>`                  |
| `push_back`             | `push` (双 API)                            |
| 24 字节（3 指针）             | 24 字节（CompressedPair EBO）                 |

### String — UTF-8 Guaranteed at the Type Level

**C++ 现状**：`std::string` 不保证 UTF-8。你可以把任意字节塞进去，验证靠自己。

**libxpp 写法**：

```cpp
// 只有两种方式获得 String：
// 1. 从已知字面量直接构造（编译期保证）
String s = String::from_utf8_unchecked("hello");

// 2. 从运行时数据构造（运行期验证）
auto r = String::from_utf8(unknown_bytes);
if (r.is_err()) { handle_bad_utf8(r.unwrap_err()); }

// 一旦拥有 String，类型就保证是合法 UTF-8
// const String& = "valid Unicode text"
// Vec<uint8_t>  = "opaque bytes, maybe not text"
```

**设计理由**：`const String&` 本身就成了「这是合法 Unicode」的证明——不在注释里，在类型里。

### Span<T> — Non-owning View

**C++ 现状**：裸指针 + 长度分开传，容易对不上。C++20 有 `std::span`。

**libxpp 写法**：

```cpp
void process(Span<const uint8_t> data) {
    for (auto& byte : data) { /* ... */ }
    // data.size()、data.begin()/end() 整装齐备
}

Vec<uint8_t> buf = /* ... */;
process(buf.as_span());           // Vec → Span，零拷贝
process(Span<const uint8_t>(ptr, len));  // 裸指针+长度 → Span
```

### 总结：什么是「同构移植」

回到 Section 11 的框架——这些类型全是**值语义层直译**：

| Rust          | libxpp (C++11)  | 同构方式                           |
| ------------- | --------------- | ------------------------------ |
| `Option<T>`   | `Option<T>`     | tagged union + bool，直译         |
| `Result<T,E>` | `Result<T,E>`   | 同上                             |
| `Vec<T>`      | `Vec<T, Alloc>` | 方法一一对应，加 Alloc 模板参数            |
| `String`      | `String`        | UTF-8 保证、chars() 迭代器、split_off |
| `&[T]`        | `Span<T>`       | 胖指针，直译                         |
| `Box<T>`      | `Box<T>`        | unique_ptr 语义，直译               |

**它们跟 Promise 的关系**：Promise 用 Option 表 "pending or ready"，用 Result 表 "success or OOM"，用 Vec 存文件内容，用 String 表 UTF-8 文本。异步机制是骨架，这些类型是血肉。缺哪一个都不完整。
