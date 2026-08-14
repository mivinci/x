# xpp 异步机制设计 — 技术分享

> 这个项目的构想诞生于 2022 年。当时在做自己的一个玩具编程语言，过程中发现 Rust 的语义设计跟 C++ 几乎同构——C++ 不缺能力，缺的是把这些能力组织起来的运行时。STL 并不完美，甚至可以说丑陋。于是有了一个想法：给 C++ 补一套机制，让它跟 Rust 一样安全、一样好用。
>
> 但工程量比想象中大得多。Rust 的 `Future` trait 怎么落到 C++11 上？下游每个模块怎么设计？模块之间怎么有机结合而不互相冲突？数学上的结构美感怎么保住？这些问题每一个都要精雕细琢。断断续续搞了几年——中间去研究了一阵 cloud native 和 LLM——始终没能一口气推到底。
>
> 直到 AI coding 水平发展到能一起做这种事，进度才指数上升。这份分享讲的就是这几年沉淀下来的设计，重点不在 API 罗列，在**为什么这么设计**。
>
> 开源地址：github.com/mivinci/x — 记得三连呀 ～～  
> 文档地址：le0.me/x

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
        .then([](xpp::http::Response&& resp) {
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

EventLoop 是一个**单线程事件循环**——在一根线程上反复 dispatch 三类事件源，直到没有活可干或被显式 stop。

### 五类事件源

| 事件源       | 注册 API                           | 触发条件                       |
| --------- | ------------------------------- | -------------------------- |
| I/O fd    | `xEventAdd(fd, mask, fn)`        | epoll/kqueue 返回 fd ready   |
| Timer     | `xTimerStart(fn, timeout_ms, …)` | 定时到期                        |
| Signal    | `xSignal(signo, fn, arg)`        | POSIX 信号到达（signalfd/self-pipe 转成事件，回调跑在 loop 线程不在 signal context） |
| Work      | `xWorkSubmit(group, work_fn, done_fn, …)` | 后台线程池跑完 `work_fn`，`done_fn` 回到 loop 线程 |
| Task      | `xEventLoopPost(fn, arg)`       | 别处 post 的回调（跨线程投递，不开线程池） |

前三类是单线程事件——fd / timer / signal 都在 loop 线程上触发。后两类都跨线程，但性质不同：

- **Work**：CPU 密集任务扔到 `xTaskGroup` 线程池，跑完后 `done_fn` 被调度回 loop 线程。`xpp::work()` 就是它的封装——Section 7 会展开
- **Task**：`xEventLoopPost` 不开线程池，只是把 `fn` 排进 loop 的 done-queue 下一轮执行。轻量跨线程通信用这个

Section 7 的 `xpp::work()` 和多 EventLoop channel 全靠这后两类。

### 三种 RunMode

`xEventLoopRun(loop, mode)` 的 mode：

| 模式              | 行为                          | 用途              |
| --------------- | --------------------------- | --------------- |
| `X_RUN_DEFAULT` | 跑到 `stop()` 或没有 active handle | 主循环 `loop.run()` |
| `X_RUN_ONCE`    | 跑一轮，阻塞到至少一个事件就返回            | `await()` 内部的 `cx.park()` |
| `X_RUN_NOWAIT`  | 跑一轮，不阻塞，没事件也立刻返回            | 集成进外部 run loop（iOS CFRunLoop / Android Looper）的手动 pump |

Section 3 会看到 `cx.park()` 内部调的就是 `X_RUN_ONCE`——跑一轮看有没有进展，没有就继续阻塞。

### EventLoop / WaitScope 分离

这是从 libuv 借来的设计：`EventLoop` 只管 handle 的 create/destroy 和 run/stop；`WaitScope` 管线程绑定（enter/leave）。**分离不是洁癖，是四个工程诉求共同逼出来的**：

| 理由        | 说明                                                                              |
| --------- | ------------------------------------------------------------------------------- |
| 移动端集成     | `xEventLoopEnter` 把 libx loop 注册到当前线程，让 iOS CFRunLoop / Android Looper 能 pump 它——这是移动端集成的唯一入口 |
| 嵌套 await 安全 | 同一线程可以嵌套多个 WaitScope（await 里面再 await），用栈管理 enter/leave 对，不会交叉污染                  |
| 线程名调试     | Enter 时设线程名（`pthread_setname_np`），Leave 时恢复——perf / lldb 里能看出线程归属              |
| 跨线程唤醒     | `wake()` / `stop()` 跨线程安全，可以叫醒正在 poll 的 loop——Section 7 多 EventLoop 拓扑的基础         |

### 跟 libuv / tokio 的关系

- **libuv**：`uv_run` 的 `UV_RUN_DEFAULT` / `UV_RUN_ONCE` / `UV_RUN_NOWAIT` 跟 xpp 三种 mode 一一对应——xpp 借了这个 API 形态
- **tokio**：多线程 runtime，worker 之间窃取任务；xpp 目前只做单线程，多线程通过"多个 EventLoop + channel 互通"实现（Section 7 讲），不学 tokio 的 work-stealing——简单优先

```cpp
// 典型用法
xpp::EventLoop loop;
{
    xpp::WaitScope scope(loop);   // enter：把 loop 绑到当前线程
    loop.run();                   // 阻塞跑到 stop() 或无 active handle
}                                // leave：解绑，线程名恢复
```

---

## 3. 核心循环：poll + waker

### 一次完整调用，端到端走读

把这一行拆开看：

```cpp
// 一次 HTTP GET，返回 Response
auto resp = xpp::http::get(url).await();
```

`http::get` 内部要做三件事：DNS 解析、TCP connect、HTTP 请求/响应读写。我们把 connect 完成后那一阶段（已经拿到 connected socket fd）放大看，poll/waker 在哪里触发。

```
xpp::http::get(url) 被调用：
│
├─ ① http::get(url)
│     → 内部拿到 connected socket fd
│     → fd 包装成 AsyncFd：fcntl 设为 non-blocking
│     → 通过 xEventAdd 注册到 EventLoop（edge-triggered，Read|Write）
│     → 返回 Promise<Response>，背后是 AdapterPromiseNode
│
├─ ② .await()
│     → 进入 poll 循环（就是下面那个 while(true) 循环）
│     │
│     ├─ 第 1 轮 poll:
│     │    AdapterNode poll：「response 数据就绪了吗？」
│     │    底层 AsyncFd 检查内部 readiness → false（还没收齐响应）
│     │    回答：Pending
│     │    副作用：waker 被注册到底层等待表里
│     │            （EventLoop 不直接知道 waker——它只管 fd→回调 的映射）
│     │    → 代码走到 cx.park()
│     │
│     ├─ cx.park()
│     │    → 调用 xEventLoopRun(ONCE)
│     │    → 最终调用 epoll_wait(timeout, ...)
│     │    → 当前线程阻塞，等内核通知
│     │
│     ├─ 内核：服务器响应到达，socket fd 变为可读
│     │    → epoll 返回这个 fd 的事件
│     │    → EventLoop 调用 AsyncFd 注册的回调（fd→回调 映射在 EventLoop 里）
│     │    → 回调里 resolve() → wake waker（查等待表，触发注册过的 waker）
│     │    → cx.park() 返回，线程醒来
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

**每一层 `.await()` 都在重复同一件事**：poll → 没好就 park → EventLoop 跑一轮 → 被内核叫醒 → poll → 拿到值。整个循环的伪代码就下面这几行。

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

> **注**：本节只讲流程层面——poll/waker 在哪里触发、park 怎么被唤醒。AsyncFd 内部用什么数据结构把「waker 注册到等待表」做对、跨线程安全性怎么保证，是 Section 4「PromiseNode 与 PromiseResolver」的话题。本节读者只需要知道：fd 注册到 EventLoop、回调里能 resolve、waker 被 wake，就够了。

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

> **注**：这是通用 Adapter 的设计。特定实现可能简化——比如 `AsyncFd` 因为是 single-threaded + 一个 fd 对应一个等待位，直接把 `PromiseResolver` 作为成员变量挂在对象上，不走 `Arc<ResolveState>`。机制本质一致，数据结构按场景裁剪。

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

Section 1 讲了三种**消费方式**（then / await / co_await）——那是 Promise 给到调用方后，调用方怎么用。本节换视角：**聚焦 `.await()` 这一支**，看它在不同上下文下行为怎么变。`co_await` 是编译器帮我写状态机，本质同源；`.then()` 不阻塞、没有「上下文」概念。只有 `.await()` 会真的把当前执行流挂起，所以它的"在哪挂、怎么醒"才有三种变体：

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
| 5 | 跨    | –     | 直接     | `xpp::work(fn)` — worker 线程跑 fn，跑完直接 resolve（worker 不是 fiber） |
| 6 | 跨    | –     | channel | 多个 EventLoop 各跑线程，channel 互通（每个线程内部可能用 fiber） |

**一个边界情况**：Adapter 模式（timer 到期 / fd 可读）的 `resolve` 来自 EventLoop 调度回调，不在任何 fiber 里。按这个分类维度，它落进 #1——await 在某 fiber 内，resolve 来自同线程的 EventLoop 回调，没有跨 fiber 也没有跨线程。严格说"被调方"不是 fiber 而是 EventLoop 本身，但值传递路径等价于 #1。

---

## 8. 性能细节：Per-Chain Arena

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

**6. 调用拓扑 — 单线程 EventLoop，多线程软件**  
EventLoop 单线程，**不等于整个软件只能跑一个线程**。六种调用拓扑覆盖同/跨线程 × 同/跨 fiber × 直接/channel 的所有组合：直接 `.await()` 链、同 EventLoop 跨 fiber、`xpp::work` 后台线程池、多 EventLoop + channel 互通——都跑在同一套 poll/waker 之上。Promise 管「做完一件事通知我」，channel 管「多个事之间怎么传数据」，两套正交互补。

**7. 零开销**  
Per-Chain Arena 让 then 链的 malloc 从 O(N) 降到 O(1)：链上所有 PromiseNode 共享一个 256 字节 bump allocator，链结束时一次 free。Timer/Work 有对象池（freelist），跨线程通信用 lock-free MPSC + Treiber stack，wake 带 coalescing。所有 debug check 在 release 优化掉。

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

---

## 13. Appendix: 范畴论视角 — Promise 是个 Monad

> 如果有时间，补这一节。没有也不影响前面的理解。

前面 12 节讲的是工程实现。但 xpp 的 Promise 背后有一个干净的数学结构——理解它，能解释为什么 `.then()`、`Promise.all`、`XPP_TRY` 这些 API "就该长这样"。

### 为什么要在乎范畴论

范畴论不是事后给工程披一件数学外衣。它给的是一个**完备性论证**：如果你要异步值具备下面五条工程能力，最终只会落到 monad 这一套结构上。

1. **异步值是一等值**——能存进变量、能当参数返回值、能放进容器
2. **串行组合**——`f()` 后接 `g()`，能写成链，不嵌套
3. **并行组合**——多个异步值能合并成一个（`Promise.all`）
4. **组合不依赖括号位置**——`(a then b) then c` ≡ `a then (b then c)`，链可重构
5. **嵌套可拍平**——异步返回异步时，`Promise<Promise<T>>` 能压成 `Promise<T>`

**这五条 = monad 三条律**。不是巧合——是范畴论保证的等价。少要一条可以走别的路子（回调缺 4、actor 缺 5、stackful coroutine 把 1-5 全藏在 runtime 里不在类型层面显式），但少了对应的工程能力。

xpp 选 Promise 这套形态，**既是冲着数学结构、也是冲着工程能力**——两者在这里是同一件事的两面：数学结构保证了工程能力不漏底，工程需求恰好落到数学结构的最小解上。这不是凑出来的 API——是异步在类型论里的标准形态。

**直觉上**：functor 是"可以往里头灌函数"的容器——容器外形不变，里面值变了。monad 是 functor 的强化，多了"把嵌套拍平"的能力。`Vec`、`Option`、`Promise` 表面天差地别，骨子里是同一个结构。

### 先回顾范畴论里的 functor 和 monad

**Functor** `F : C → D` 是"范畴到范畴的映射"——把对象映到对象、态射映到态射，**保持复合和单位元**：

- 对象 `X ∈ C`  ↦  `F(X) ∈ D`
- 态射 `f : X → Y`  ↦  `F(f) : F(X) → F(Y)`
- 满足两条律：
  - `F(g ∘ f) = F(g) ∘ F(f)`（保持复合）
  - `F(id_X) = id_{F(X)}`（保持单位元）

**Monad** 是**范畴到自身的 functor**（endofunctor）`T : C → C`，再加两个自然变换：

- `return : A → T(A)`（把值装进容器）
- `bind : T(A) → (A → T(B)) → T(B)`（等价于 `fmap + join`，`join : T(T(A)) → T(A)` 负责拍平嵌套）

满足三条律：左单位、右单位、结合律（下面展开）。

**编程里的特例**：只用 `C = D = Type` 范畴（对象是类型，态射是函数）。所以编程里说的 functor/monad 都是 `Type → Type` 的 endofunctor——源范畴和目标范畴是同一个。

### Promise 套进 functor 定义

把范畴论定义逐条对应到 Promise：

| 范畴论 | Promise 里的写法 |
|---|---|
| 范畴 C | `Type` 范畴（对象是类型，态射是函数） |
| 对象 X | 类型 `int`、`string`、`Response` |
| 态射 `f : X → Y` | 函数 `f : int → string` |
| 复合 `g ∘ f` | 函数复合 `g(f(x))` |
| `id_X` | `[](auto x) { return x; }` |
| Functor F | `Promise<_>` 类型构造子 |
| 对象映射 `F(X)` | `Promise<int>` |
| 态射映射 `F(f)` | `fmap(f) = [p](Promise<int> p) { return p.then(f); }` |

两条 functor 律对应到具体行为：

**律 1 `F(g ∘ f) = F(g) ∘ F(f)`** —— "先合成再 then" 等价于 "一个一个 then"：

```cpp
// 左边：先把 f、g 合成成一个函数，再 then
p.then([](int x) { return g(f(x)); })

// 右边：先 then f，再 then g
p.then(f).then(g)

// 两者行为一致
```

**律 2 `F(id_X) = id_{F(X)}`** —— "then 一个什么都不做的函数" 等于 "没 then"：

```cpp
p.then([](auto x) { return x; })   // 等于 p 本身
```

**律的实用含义**：Promise 是个透明容器，`then` 不偷偷加副作用、不偷偷改状态。你能放心地 `then` 来 `then` 去，不会出现"咦怎么行为变了"。

### Promise 套进 monad 定义

Functor 只能处理"函数返回普通值"——`f : A → B` 被 `fmap` 提升成 `F(A) → F(B)`。但当函数自己返回容器时 `f : A → F(B>`，`fmap` 会得到 `F(F(B>>`（套两层）。

Monad 多一步 `join : F(F(B>> → F(B>`，把嵌套压扁。`bind = fmap + join`。

Promise 的对应：

| 范畴论 | Promise 里的写法 |
|---|---|
| endofunctor `T : Type → Type` | `Promise<_>` |
| `return : A → T(A)` | `xpp::resolve(a)` |
| `bind : T(A) → (A → T(B)) → T(B)` | `p.then(f)` 当 `f : A → Promise<B>` |
| `join : T(T(A)) → T(A)` | `ChainPromiseNode`（自动把 `Promise<Promise<B>>` 拍平成 `Promise<B>`）|

三条 monad 律：

| 律 | 形式 | xpp 含义 |
|---|---|---|
| 左单位 | `resolve(a).then(f) ≡ f(a)` | 已有值再 then，等于直接调 f |
| 右单位 | `p.then(resolve) ≡ p` | 把值再包一层，等于没包 |
| 结合律 | `(p.then(f)).then(g) ≡ p.then(x => f(x).then(g))` | 链怎么括不影响结果 |

**`.then()` 既是 `fmap` 又是 `bind`** —— C++ 重载了。普通函数走 fmap 路径；返回 Promise 的函数走 bind+join 路径（`ChainPromiseNode` 把 `P(P(B))` 拍平成 `P(B)`）。这是为什么 `.then()` 链不会出现 `Promise<Promise<Promise<T>>>`。

### Async 函数是 Kleisli 箭头

同步世界是 **Set** 范畴：对象是类型，态射是 `A → B`。

异步世界是 **Kleisli 范畴 Kl(P)**：对象还是类型，态射是 `A → P(B)`（叫 Kleisli 箭头）。

- 单位态射：`resolve : A → P(A)`
- Kleisli 复合 `>=>`：`(f >=> g)(a) = f(a).then(g)`

**你写 `f().then(g)` 时，你在做 Kleisli 复合** —— 只是写法是链式而不是函数式。

### `.await` 是 direct-style 语法糖

`.await : P(A) → A` 看起来是从 monad 里"逃出来"——但它在纯函数式里写不出来（会破坏 referential transparency）。

`async/await` 的本质：**编译器把 direct-style 代码 desugar 成 Kleisli 复合**。你写：

```cpp
auto a = f().await();
auto b = g(a).await();
return h(b);
```

编译器翻译成：

```cpp
return f().then([](A a) {
    return g(a).then([](A b) {
        return resolve(h(b));
    });
});
```

完全是 Kleisli 复合。`await` 让程序员不用手写 `.then()` 嵌套——这就是"语法糖"的精确含义。

Rust 的 `?` 错误传播操作符也是同一个东西，只是作用在 `Result` monad 而不是 `Promise` monad。结构完全同构。

### 其他 async 操作都是 monad 的衍生

| 操作 | 范畴论名字 | 类型 |
|---|---|---|
| `Promise.all([p1, ..., pn])` | `sequence` (Applicative) | `List<P(A)> → P(List<A>)` |
| `Promise.race(p1, p2)` | `Alternative` 的 `<\|>` | `P(A) × P(A) → P(A)` |
| 错误传播 `XPP_TRY` | `MonadError` | `P<Result<T, E>>` 的短路传播 |

`all` 是 `traverse`/`sequence`——把"列表 of monad"翻成"monad of list"。`race` 是 `Alternative`——两个 monad 值选一个。**这些都不是凭空加的 API，是 monad 结构自然衍生的**。

### CPS 是"所有 monad 之母"

回调形式 `A → (B → R) → R` 就是 **continuation monad** `Cont R`。

范畴论有个深刻结果：**任何 monad 都能用 CPS 编码**（通过 `call/cc`）。所以：

- Promise 是 CPS 的 reification（把 continuation 物化成值）
- Future/Task 也是 CPS 的 reification，细节不同
- async/await 是 CPS 的 syntax sugar

它们在范畴论里都是同一个构造的不同呈现。**Section 0 讲的"回调 vs Promise 形式上等价"**，本质就是这个——CPS 是共同的根。

### Promise 在 monad 家族中的位置

xpp 里这些类型全是 monad，各自承载一种"副作用"：

| 类型 | "副作用" |
|---|---|
| `Option<T>` | "可能没有值" |
| `Result<T, E>` | "可能失败" |
| `Vec<T>` | "可能有多个值" |
| `Promise<T>` | "值会晚到" |
| Haskell `IO<T>` | "依赖外部世界" |

都有 `return` 和 `bind`，都形成 Kleisli 范畴。**Promise 是这个家族里"时间维度"的那一个**。

### 一句话

> **xpp 的 Promise 是 `Promise` monad 在 C++11 上的实现。`.then()` 既是 `fmap` 又是 `bind`，`async/await` 是 Kleisli 复合的 direct-style 语法糖。这不是凑出来的 API——是异步在类型论里的标准形态。**
