# Issue: 捕获 lambda 协程 + 嵌套 spawn → resume 崩溃（协程帧捕获损坏）

> 状态：**已定位并修复（2026-08-24）**——原崩溃根因为 **lambda 协程帧存储闭包指针而非闭包拷贝**（语言/工具链语义）+ 闭包生命周期不足（用户侧约束，已文档化）；排查过程中另发现并修复 **两个真实 xpp bug**（ChainPromiseNode 提前析构闭包、协程帧泄漏）。
> 发现于：pi/http-server 分支排查流式响应 coroutine 测试时（提交 `7535e1c`）
> 影响：`xpp::spawn` 的 poll 回调内再 spawn 一个**捕获 lambda 协程**时会崩

## 根因（已确认）

Apple Clang（21.0.0, arm64）为 lambda 协程生成的帧存储的是**闭包对象的指针（`this`）**，而不是闭包的拷贝。最小验证：

```cpp
auto make = [px]() -> Task { if (px) ...; co_return; };
Task t = make();
// dump 帧内容：frame+24 == &make（闭包地址），而 px（&x）只存在于 *(&make) 中
```

复现路径中，`make` 是外层**普通 lambda**（非协程，在 `TransformPromiseNode::poll` 内同步执行）的**栈局部变量**。嵌套 `spawn(make())` 后：

1. 协程帧记录 `this = &make`（外层 lambda 的栈槽）；
2. `xEventLoopPost` 把内层 `spawn_step` 延迟到事件循环；
3. 外层 lambda 返回、栈帧消亡，`make` 所在栈槽被后续调用复用；
4. 延迟的 `spawn_step` → `poll` → `m_handle.resume()` 通过悬空的 `this` 读捕获 → 读到陈旧栈内容（`0x8` / `0x7000020000` / 代码地址）→ SEGV/BUS。

之前 dump 的 "frame+24 = 正确栈地址" 实为 `&make`（看起来合法的栈地址），并非 `&ran` 本身；其指向内容在 resume 时早已是垃圾。所有对照实验均可由此解释：

| 实验 | 解释 |
| --- | --- |
| 同步 poll 正常 | 外层 lambda 栈帧（含 `make`）仍存活 |
| 无捕获 lambda 正常 | 悬空 `this` 从未被解引用 |
| 命名协程函数正常 | 函数**参数**按标准拷贝进协程帧 |
| TEST 顶层创建正常 | `make` 在 TEST 栈帧中，寿命覆盖整个 loop run |
| 崩溃值每次不同 | 陈旧栈内容不确定 |

**结论**：这是标准 C++ 语义下的闭包生命周期错误（协程由普通成员函数的隐式对象参数指针引用闭包），等价于"把栈上闭包的协程交给运行时异步驱动"。无编译器问题可报（行为符合语言规则；MSVC 拷贝闭包属于已知的实现差异）。

## 安全模式（已写入 `spawn.h` 文档与回归测试）

- `xpp::spawn([&]() -> Promise<void> { ... })`（**直接传 lambda**）：安全——defer 节点把闭包**按值存到堆上**（`TransformPromiseNode::m_fn`），且（修复第二个 bug 后）闭包存活到链完成；
- `auto make = [&]{...}; spawn(make());`：要求 `make` **存活到链完成**；
- 命名协程函数：参数拷贝进帧，天然安全。

## 顺带修复的真实 xpp bug：协程帧泄漏

`CoroutinePromiseNode::~CoroutinePromiseNode()` 原为 `if (m_handle && !m_done) m_handle.destroy();`——协程正常完成（`m_done=true`）后停在 final_suspend，析构跳过 `destroy()`，**每个完成的协程帧都泄漏**。已改为 `if (m_handle) m_handle.destroy();`（`libxpp/xpp/promise_coroutine.h`）。

## 第二个真实 xpp bug：ChainPromiseNode 提前析构闭包（使"直接传 lambda"此前实际上是坏的）

把 server_test 改为直接传 lambda 验证时暴露：`spawn(lambda)` / `.then(fn)` 的展平路径中，
`ChainPromiseNode::poll` 取出内层 Promise 后执行 `m_outer = nullptr`，**立即析构
TransformPromiseNode 及其持有的 Func（lambda 闭包）**——而被展平的内层协程帧仍通过
`this` 引用该闭包。后果（取决于捕获类型）：

- RAII 捕获（如 `mpsc::Sender`）：析构提前发生 → sender 计数归零 → 通道被自动关闭 →
  协程体"正常"跑完但数据从未入队（无崩溃、无 ASan 报告——arena 内存未释放且对象内存
  完好，极难发现；最小复现：协程体内同步 `try_send` 返回 Closed）
- 裸指针捕获（`[&ran]`）：读已析构但内存完好的闭包 → 碰巧"正常"，掩盖了 bug

修复：`ChainPromiseNode::poll` 保留 `m_outer` 所有权直到链完成（`m_inner` 置位后不再
poll 它，仅延长生命周期）。`libxpp/xpp/promise_node.h`。

## 落地变更（2026-08-24）

- `libxpp/xpp/promise_node.h`：**修复 ChainPromiseNode 提前析构 TransformPromiseNode/闭包**（第二个 bug）
- `libxpp/xpp/promise_coroutine.h`：修复 `~CoroutinePromiseNode` 帧泄漏
- `libxpp/xpp/spawn.h`：文档化"协程 lambda 与闭包生命周期"约束
- `libxpp/xpp/spawn_test.cpp`：新增回归测试 `NestedSpawnCaptureLambdaCoroutine`（闭包存活的安全嵌套 spawn）、`SpawnedLambdaCoroutineClosureSafe`（直接传 lambda）
- `libxpp/xpp/http/server_test.cpp`：`CoroutineStreamingProducer` 改为直接传 lambda 协程（验证安全模式），`CoroutineHandlerStreamingResponse` 保留命名函数写法——两种安全模式各有覆盖
- 验证：xpp 69/69、libx 80/80 通过（ASan Debug）

---

以下为原始排查记录。

## 现象

捕获 lambda 协程从另一个 spawn 链的 **poll 回调内部**再 `xpp::spawn`（嵌套 spawn），协程首次 resume 时崩溃（ASan 下 SEGV/BUS），**捕获引用读到错误值**——每次运行不稳定：`0x8` / `0x7000020000` / 代码段地址（如 `0x1023bf920`）。

## 最小复现

```cpp
// libxpp/xpp/http/coro_probe_test.cpp（临时探针，已删除）
#include <gtest/gtest.h>
#include <xpp/event.h>
#include <xpp/promise.h>
#include <xpp/spawn.h>
using namespace xpp;

TEST(CoroProbe, NestedSpawnCoroutine) {
  EventLoop loop; WaitScope scope(loop);
  bool ran = false;
  xpp::spawn([&ran]() -> Promise<void> {          // 外层 spawn 链
    auto make = [&ran]() -> Promise<void> { ran = true; co_return; };  // 捕获 lambda 协程
    xpp::spawn(make());                            // 嵌套 spawn
    ran = true;
    return xpp::resolve();
  });
  for (int i = 0; i < 100 && !ran; ++i) xEventLoopRun(loop.handle(), X_RUN_ONCE);
  EXPECT_TRUE(ran);
}
```

ASan 崩溃栈（关键帧）：

```text
#0  …::'lambda'()::operator()() const (.resume)   coro_probe_test.cpp:11  (ran = true;)
#2  xpp::_::CoroutinePromiseNode<void>::poll(PromiseContext const&)  promise_coroutine.h:104  (m_handle.resume())
#3  xpp::_::spawn_step<void>(void*)                spawn.h:66   (st->node->poll(cx))
#4  loop_run_done                                  event_private.c:128 (post_fn(w->arg))
SUMMARY: AddressSanitizer: SEGV/BUS
```

## 环境

- macOS（Apple Silicon，arm64e），Xcode Clang
- C++20（`XPP_HAS_COROUTINES=1`，`libxpp/xpp/compiler.h`）；`XPP_FIBER` 未启用
- 构建：`cmake -B build -G Ninja && cmake --build build -j --target http_coro_probe_test`（ASan；**Release 无 ASan 同样崩**）
- C++11 构建不受影响（协程代码在 `#if XPP_HAS_COROUTINES` 内）

## 相关代码

| 文件 | 要点 |
| --- | --- |
| `libxpp/xpp/spawn.h` | `spawn(Promise)` / `spawn(Func)`（`defer` 包装）/ `spawn_impl`（`xEventLoopPost` 延迟驱动）/ `spawn_step`（poll + `delete st`） |
| `libxpp/xpp/promise_coroutine.h` | `CoroutinePromiseNode<T>`（`poll` while 循环 + `m_handle.resume()`）/ `CoroutinePromise`（promise_type，`get_return_object`）/ awaiter（`await_ready` 恒 false，`await_suspend` → `set_await`） |
| `libxpp/xpp/promise_node.h` / `promise_allocator.h` | 节点类型与 `promise::allocate`（heap-only，无 arena） |
| `libx/x/base/event_private.c` | `loop_run_done`：pop done queue → `post_fn(w->arg)` → `event_work_free` |
| `libx/x/base/event_post.c` | `xEventLoopPost`：`event_work_alloc`（freelist 复用/calloc）→ `xMpscPush` |
| `libxpp/xpp/http/server.h` | `srv_on_request_cb` 用 `xpp::spawn` 驱动 handler（场景来源） |

## 关键机制（背景）

`spawn_impl`（`spawn.h`）：

1. `async<T>()` 创建 JoinHandle
2. `_extract_node(std::move(p))` 取出节点
3. `new SpawnState{node, waker, resolver}`，`set_wake_arg(st)`
4. `xEventLoopPost(xEventLoopCurrent(), &spawn_step<T>, st)` —— **post 到事件循环 done queue，延迟执行**
5. `spawn_step`（由 C 代码 `loop_run_done` 经 `post_fn` 函数指针调用）：`PromiseContext cx(st->waker); st->node->poll(cx)`；完成则 `resolve_spawn_result` + `delete st`

协程 promise（`promise_coroutine.h`）：

- `get_return_object()`：`promise::allocate<CoroutinePromiseNode<void>>`，`n->m_handle = std::coroutine_handle<CoroutinePromise<void>>::from_promise(*this)`，返回 `Promise(OwnPromiseNode(n))`
- `initial_suspend()/final_suspend() = suspend_always`（创建即挂起，poll 才 resume）
- `CoroutinePromiseNode::poll`（`promise_coroutine.h:88`）：

```cpp
Option<ValueType> poll(const PromiseContext &cx) override {
  while (true) {
    if (m_done) { … return result; }
    if (m_await_state) {
      if (!m_await_state->poll(cx)) return none;
      m_await_state.reset();
      m_handle.resume();
    } else {
      m_handle.resume();   // 首次 resume（line 104 附近）
    }
  }
}
```

- `~CoroutinePromiseNode()`：`if (m_handle && !m_done) m_handle.destroy()`

## 已完成的诊断（排除项）

| 假设 | 结果 |
| --- | --- |
| ASan 布局影响 | ❌ Release 构建同样崩 |
| 节点/frame 生命周期（被释放/移动） | ❌ 打印确认 node=`0x606000000c90`、frame=`0x604000002610` 创建→poll 之间完全不变 |
| 捕获初始化失败 | ❌ 创建时（get_return_object 后）dump frame：`frame+24 = 0x16f…`（正确栈地址，捕获已写入） |
| 分配器冲突（`xWork_` calloc/freelist vs frame `operator new`） | ❌ 不同堆区域（0x6040… / 0x6030… / 0x6060…） |
| 纯 C++20 协程对照（无 xpp） | ❌ 完全正常——包括协程内嵌套驱动、经中间普通函数延迟 resume、捕获 `&ran` 均正确 |
| **同步 poll（临时把 `spawn_impl` 的 `xEventLoopPost` 改为直接 `spawn_step(st)`）** | ✅ **恢复正常**（`&ran` 读到正确栈地址） |

其他对照实验（探针文件已删，均在 `#if XPP_HAS_COROUTINES` 测试中验证）：

- spawn + 协程（TEST 顶层创建，非嵌套）：正常
- 嵌套 spawn + 非协程 promise：正常
- 嵌套 spawn + **无捕获** lambda 协程：正常
- 嵌套 spawn + **命名协程函数**（含 `co_await mpsc::send` 循环）：正常
- 嵌套 spawn + **捕获** lambda 协程：崩溃 ← 唯一失败组合

## 定位结论

**触发条件精确为**：协程创建在 xpp spawn 链的 **poll 回调内部**（`TransformPromiseNode::poll` → 外层 fn）+ **经 `xEventLoopPost` 延迟到 loop 的 C 回调栈（`loop_run_done` → `post_fn`）再 resume**。同步立即 poll 则正常。

**矛盾点（关键线索）**：同一次运行中，poll 前 dump `frame+24` 是正确栈地址，紧接着 `m_handle.resume()` 后协程体第一条语句读 `&ran` 却得到错误值（且每次运行值不稳定）。frame 内容在 resume 前后应不变——因此指向 **编译器生成的协程 resume 序言在 C 回调调用栈（C→C++ 函数指针调用链）下的帧指针/捕获偏移恢复问题**（ABI 层），而非 xpp 的节点/调度/生命周期逻辑错误。

frame 布局 dump（创建时，32 字节；ASan 下 `frame+32` 为未初始化 0xbe，且**读 frame+40 会触发 ASan 越界**——frame 大小 ≈ 32 字节）：

```text
frame+0  = 0x100…（resume 函数指针）
frame+8  = 0x100…（destroy 函数指针）
frame+16 = 0x606000000c90（promise_type.m_node）
frame+24 = 0x16f…（捕获 &ran —— 创建时正确）
```

## 建议排查方向

1. **对比同步 poll（正常）与异步 post（崩溃）两条路径的调用栈差异**，重点：`loop_run_done`（C 代码）→ `post_fn` → `spawn_step`（C++）→ `poll` → `m_handle.resume()` 的 C→C++ 跨越点是否破坏编译器协程 resume 序言依赖的栈/寄存器状态
2. **lldb 单步 `m_handle.resume()` 的序言**（`promise_coroutine.h` line 104；上次 lldb 批处理超时，建议 `-o "breakpoint set --file promise_coroutine.h --line 104"` 交互式断点），对比同步/异步两条路径 `frame+24` 的读取偏移
3. **检查 `xEventLoopRun`/`loop_run_done` 是否在回调前做了非常规栈操作**（`libx/x/base/event_private.c` 的 `loop_run_done`、`event_post.c` 的 `xEventLoopPost`）
4. 若确认编译器问题：剥离 xpp 依赖的最小复现，考虑报 LLVM

## 当前状态 / 绕开方式

- **已定位并文档化**（见顶部"根因"），修复见"落地变更"
- `server_test.cpp` 继续使用命名协程函数 `co_stream_producer`（本就是安全模式）
- 回归测试已加入 `spawn_test.cpp`；探针文件已删除，工作区干净

## 复现步骤（给接手者）

```bash
# 1. 重建探针测试文件 libxpp/xpp/http/coro_probe_test.cpp（内容见"最小复现"）
# 2. cmake -B build -G Ninja && cmake --build build -j --target http_coro_probe_test
# 3. ./build/libxpp/xpp/http_coro_probe_test --gtest_filter=CoroProbe.NestedSpawnCoroutine
#    （ASan 下 SEGV/BUS；Release 构建同样崩溃）
# 4. 验证绕开：把 make 改为命名协程函数或 []() 无捕获 lambda → 测试通过
```
