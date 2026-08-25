# Issue: mpsc 单槽 waiter 竞态 —— TrySendRecvWorkerThread 偶发挂起

> 状态：**已修复（2026-08-24）**——三层修复（驱动层 late-wake 安全 → mpsc 重写为 tokio 式 waiter 协议 → libx C 层预存竞争），ASan 80/80 + TSan 0 警告，见文末"修复记录"
> 影响：`xpp::sync::mpsc` 跨线程 send + 事件循环线程 recv（挂起）的并发场景，偶发**永久挂起**（丢失唤醒）
> 复现：`MpscMtTest.TrySendRecvWorkerThread`（ASan 构建历史 ~45% 概率，9/20）

## 现象

`libxpp/xpp/sync/mpsc_test.cpp` 的 `MpscMtTest.TrySendRecvWorkerThread`：

```cpp
auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);

std::thread worker([&tx] {
  for (int i = 0; i < 4; ++i) {
    auto r = tx.try_send(i);
    EXPECT_TRUE(r.is_ok()) << "try_send " << i << " failed";
  }
});

xpp::EventLoop loop;
xpp::WaitScope scope(loop);
auto recv_all = [&]() -> xpp::Promise<void> {
  for (int i = 0; i < 4; ++i) {
    auto v = co_await rx.recv();
    EXPECT_TRUE(v.is_some());
  }
  co_return;
};
recv_all().await();   // ← 偶发永久挂起（超时后测试被杀）
```

worker 线程 `try_send` 与事件循环线程 `recv`（channel 空时挂起）并发 → 偶发**丢失唤醒**，`recv` 永久挂起。ASan 构建下历史概率 ~45%（9/20 挂）；非 ASan / 时序变化时可能长期不触发（近期 0/30——窗口依赖精确交错）。与 spawn/http-server 改动无关（main 上同样复现）。

## 根因（已定位）

### 竞态 A：check-then-act 丢失唤醒（挂起的直接原因）

`libxpp/xpp/sync/mpsc.h` 的 `Chan` 用**单槽** `PromiseResolver` 存接收者 waiter：

```cpp
struct Chan {
  list::Tx<T>                  m_tx;
  list::Rx<T>                  m_rx;
  PromiseResolver<void>        m_read_waiter;    // 单槽，非原子
  PromiseResolver<void>        m_write_waiter;   // 同上（send 挂起路径）
  xpp::loom::_::Atomic<size_t> m_sender_count{1};
  xpp::loom::_::Atomic<bool>   m_closed{false};
  ...
};
```

`recv()`（C++20 版，`mpsc.h` ~212 行）与 `send()`（~195 行）的跨线程交错：

```cpp
// recv（事件循环线程）：
while (true) {
  auto v = m_chan->m_rx.try_pop();
  if (v.is_some()) { ...; co_return; }
  if (closed() && empty) co_return none;
  auto pr = xpp::async<void>();
  m_chan->m_read_waiter = std::move(pr.second);   // (a) 注册 waiter
  co_await std::move(pr.first);                     // (b) 挂起
}

// send（worker 线程）：
while (true) {
  if (closed()) co_return;
  if (m_chan->m_tx.try_push(value)) break;          // (c) 数据入队
  ...
}
if (m_chan->m_read_waiter.is_pending()) {           // (d) 检查 waiter
  auto w = std::move(m_chan->m_read_waiter);
  w.resolve();                                       // (e) 唤醒
}
```

**丢失唤醒时序**：

```text
recv:  try_pop → 空
send:  try_push → 成功（数据入队）
recv:  (a) 注册 m_read_waiter → (b) 挂起
send:  (d) is_pending() == false（已在 push 后、recv 注册前检查过）→ 跳过 (e)
→ recv 永久挂起（数据在队列里但无人唤醒）
```

`recv` 的"检查队列空 → 注册 waiter"与 `send` 的"入队 → 检查 waiter"之间没有同步——check-then-act 窗口。

### 竞态 B：`m_read_waiter` 跨线程非原子访问（数据竞争 UB）

即使没有丢失唤醒，(a) 的**写**（事件循环线程）与 (d)(e) 的**读 + `std::move`**（worker 线程）也是**非原子跨线程访问同一对象**——数据竞争（UB）。TSan 可检测（需两次访问实际交错）；`PromiseResolver` 内部含非原子状态，`is_pending()`/赋值/move 并发即 UB。

### 已排除：`WakeState::woken` 不是数据竞争源（修正旧结论）

`promise_waker.h` 的 `WakeState { bool woken = false; ... }` 曾被怀疑为跨线程写。实际检查 `PromiseWaker::wake()`（`promise_waker.h:189`）：

```cpp
inline void PromiseWaker::wake() const {
  if (m_state->loop == xEventLoopCurrent()) {
    do_wake(m_state.get());            // 同线程：直接写 woken
  } else {
    xEventLoopPost(m_state->loop, &on_wake, m_state);  // 跨线程：post，由 loop 线程写 woken
  }
}
```

**跨线程 wake 走 `xEventLoopPost`，`woken` 的写入始终发生在事件循环线程**（`do_wake`/`on_wake` 都在 loop 线程）——与 `promise_context.h` 的 `park()` 循环（也在 loop 线程）读写 `woken` **不构成数据竞争**。

这解释了历史修复尝试为何不彻底：**"woken 原子化"改善 ~50% 是内存序/时序的偶然效应，不是根因**——真正的根因是 `m_read_waiter` 单槽的 check-then-act（竞态 A）与跨线程非原子访问（竞态 B）。

## 历史修复尝试（未根治）

| 尝试 | 结果 |
| --- | --- |
| `WakeState::woken` 原子化（`std::atomic<bool>`） | ~50% 改善（偶然，非根因——见"已排除"） |
| `m_read_waiter` 槽加 mutex 锁 | ~40% 改善 |
| 两者组合 | 残余 ~25% |

残余说明：即使锁住 waiter 槽，若 recv 的"try_pop → 注册"与 send 的"push → 唤醒"仍跨锁分离，check-then-act 窗口依旧存在——**修复必须让"检查队列空 + 注册 waiter"与"入队 + 检查/唤醒 waiter"处于同一次同步临界区**（或等价的无锁 check-and-set）。

## 相关代码

| 位置 | 说明 |
| --- | --- |
| `libxpp/xpp/sync/mpsc.h:126-127` | `Chan::m_read_waiter` / `m_write_waiter`（单槽，非原子） |
| `mpsc.h:~195-206` | `send()` 入队后检查/移动/唤醒 `m_read_waiter`（worker 线程读） |
| `mpsc.h:~212-231` | `recv()` 空队列时注册 `m_read_waiter`（loop 线程写）后挂起 |
| `mpsc.h:~257-263` / `~322-338` | C++11 `.then()` 路径同样的 waiter 模式 |
| `mpsc.h:~495-555` | 非协程 `Receiver::recv` 的 waiter 路径（同模式） |
| `libxpp/xpp/promise_waker.h:189-201` | `wake()` 跨线程走 `xEventLoopPost`（`woken` 仅在 loop 线程写） |
| `libxpp/xpp/promise_context.h:96-100` | `park()` 非 fiber 路径：`while (!woken) xEventLoopRun(...)` |
| `libxpp/xpp/sync/mpsc_test.cpp:282` | `MpscMtTest.TrySendRecvWorkerThread`（复现） |

## 修复方向（建议）

1. **消除 check-then-act**：waiter 注册与唤醒必须原子化——方案：
   - **mutex 保护队列检查 + waiter 槽**：`recv` 在锁内「try_pop → 空则注册 waiter」，`send` 在锁内「push → 检查/移动/唤醒 waiter」——锁保证临界区互斥，窗口消除；
   - **或单槽原子指针 + CAS**：waiter 用 `Atomic<PromiseResolver*>`，recv CAS 注册、send CAS 取出唤醒；
   - 注意 **`m_write_waiter`（send 挂起路径）同样模式**——一并处理。
2. **修复后 TSan 回归**：数据竞争检测需要并发交错实际发生——用压力测试（多轮短交错：每轮 cap=1、recv 挂起与 try_send 并发）放大窗口；TSan 是正确工具（ASan 不检测数据竞争）。
3. **历史"woken 原子化"改动可保留或回退**——它不修根因；若保留需确认 `WakeState::woken` 无其他跨线程访问路径。

## 复现步骤（给接手者）

```bash
# 1. ASan 构建下循环跑（历史 ~45% 挂，近期时序漂移后概率低，需多轮）：
for i in $(seq 1 50); do
  timeout 30 ./build/libxpp/xpp/sync_mpsc_test --gtest_filter=MpscMtTest.TrySendRecvWorkerThread
  [ $? -eq 124 ] && echo "HANG at run $i"
done

# 2. TSan 构建（数据竞争检测）：
cmake -B build-tsan -G Ninja -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -DCMAKE_C_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan -j --target sync_mpsc_test
./build-tsan/libxpp/xpp/sync_mpsc_test --gtest_filter=MpscMtTest.*

# 3. 压力放大（临时探针）：每轮 cap=1，recv 挂起与 worker try_send 并发，
#    数十轮交错即可显著放大竞态窗口；TSan 下可捕获竞态 B 的 data race。
```

## 确认记录（2026-08-24，独立复核）

**结论：两个竞态均成立，issue 定位正确。**

### 代码核对

- **竞态 A**：`recv()` 的 `try_pop() → 注册 m_read_waiter`（mpsc.h:215-227）与 `try_send()` 的 `try_push() → is_pending() 检查`（mpsc.h:319-325）之间无任何同步——check-then-act 窗口实存。`send()`（:198-208）、`close()`（:322-340）、`try_recv()`（:348-351）同一模式。底层 `list::Tx/Rx`（list.h）本身线程安全（`m_count`/`m_wpos` 原子 + slot ready 标志），窗口完全在 mpsc 层。
- **竞态 B**：`PromiseResolver` 内部的 `resolve()`/`is_pending()` 各自用了原子（promise_adapter.h:125/:140），但竞态在**对象成员本身**——recv 线程对 `Chan::m_read_waiter` 的 move 赋值（写非原子 `ArcWeak m_weak` 指针，promise_adapter.h:115 + :144-145）与 worker 线程的 `is_pending()`（读同一成员）并发，标准意义上的数据竞争 UB。
- **"woken 非竞态"修正**：核对 `PromiseWaker::wake()`（promise_waker.h:189-202）属实——跨线程 wake 走 `xEventLoopPost`，`woken` 只在 loop 线程读写，不构成数据竞争。

### 复现

- 现有测试循环 30 次：0 挂起（与"近期时序漂移"一致——窗口依赖精确交错）。
- 压力探针（每轮独立 channel(4)、`tx` 保持在主线程存活防止 drop 自动 close 掩盖挂起、worker 线程 `try_send×4` 与 loop 线程 `recv×4` 并发、连续轮次）：**~500-1000 轮内必现永久挂起**（ASan Debug，timeout 124）。探针源码要点：`tx` 按引用捕获存于主线程，丢唤醒后无 close 兜底 → `recv_all().await()` 永久阻塞。

### 修复确认要点（同"修复方向"）

- 必须消除 check-then-act：锁内「try_pop → 空则注册」与「push → 检查/唤醒」互斥，或单槽原子 CAS；
- `m_write_waiter` 对称处理（send 挂起 / try_recv / close 路径）；
- 修完用本探针 + TSan 回归（探针已验证可在百轮量级稳定触发）。

## 修复记录（2026-08-24）

按依赖顺序三层落地（架构对齐 tokio，热路径零锁）：

### 第 1 层：驱动层 late-wake 安全（前提）

多次唤醒源（通道）会与等待者完成并发，产生 post-completion poll——旧 `spawn_step` 直接 `delete st` → UAF。修复：

- `SpawnState` 增加 `done` 标志 + 引用计数；每个 post 的 step 持有一个引用（post 前 retain）；`WakeState` 增加 `wake_retain/wake_release` 钩子，waker 持有 SpawnState 所有权引用；完成后 `st->waker.reset()` 打破循环引用。迟到的 step 是 no-op（`spawn.h` / `promise_waker.h`）
- `AtomicPromiseWaker` 补 `take_waker()`（tokio parity）：poll 节点复查成功后清除登记，避免陈旧 waker 被 fire 到已销毁的 loop
- 跨线程 `wake()` 的 post 从裸 `WakeState*` 改为持有引用的 WakeToken（消除同族 UAF 隐患）

### 第 2 层：mpsc 重写（tokio 式 waiter 协议）

- `Chan::m_read_waiter`（单槽 PromiseResolver）→ `AtomicPromiseWaker m_rx_waker`：recv 统一为 `RecvPromiseNode`（level-triggered poll：pop → register → 再 pop → Pending），push 后 `wake()`——与 tokio chan.rs 同构，check-then-act 窗口由状态机关闭
- `m_write_waiter`（单槽，多 producer 会互相覆盖）→ mutex + 侵入式双向链表 FIFO：push 尝试与登记在同一临界区（满载冷路径才取锁；send 快路径 try_push 完全无锁）；recv 每 pop 唤醒队首一个 sender，FIFO 无饥饿
- C++20 协程版与 C++11 递归 `.then` 版统一为同一 poll 节点实现（mpsc.h 反而变简单）
- 顺带修复：`UnboundedSender::try_send` 完全不唤醒 recv 的确定性丢唤醒 bug
- 用 `loom::_::Mutex`（库内别名）与侵入式链表，不引入 std 容器

### 第 3 层：libx C 层预存竞争（首次跑 TSan 暴露）

- `mpsc.c`：`_tail->next = node` 普通写 → release 原子写（该写是 work 结构字段的发布点，普通写破坏 happens-before 链）
- `event_run.c` / `event_private.h`：`done_head` 两处普通读 → `xMpscEmpty()` 原子读
- `list.h` unbounded：`Node::next` 原子化（push release-store / pop acquire-load）

### 验证

- 压力探针（修复前 500~1000 轮内必现永久挂起）：修复后 **50,000 轮零挂起**（ASan）
- 新增回归测试：`MpscMtTest.StressLostWakeupRegression`（2000 轮交错，78ms）、`MpscMtTest.MultipleSuspendedSenders`（cap=1 三 sender 挂起 FIFO 排干）
- TSan（首次配置）：`sync_mpsc_test` 19/19、`spawn_test` 8/8，**0 数据竞争警告**（修复前 26 个，其中 xpp 层 waiter 竞争已全部消除，其余为 libx C 层预存问题并一并修复）
- ASan 全量：80/80 通过

## 姊妹修复：Notify 的同族丢唤醒（2026-08-24，直接修复）

`sync/notify.h`（watch/broadcast 构建于其上）存在同族但形态不同的竞态：
`notify_one`/`notify_waiters` 的 `m_pending.fetch_add` 在**锁外**执行——与
`notified()` 锁内 relaxed 复查之间无 happens-before，"空列表→累计 pending"与
"复查 pending→登记 waiter"未互斥 → 同时到达的等待者读到 0 → 挂起，通知无人
消费（永久睡死）。`m_pending` 本身是原子，无数据竞争，**TSan 不可见**——纯顺序竞态。

修复：fetch_add 移入临界区（与 mpsc 修复的 mutex 融合同理）。

回归测试 `NotifyMtTest.StressPendingVsRegister`：已验证旧代码在此测试上真实
挂起（timeout kill）；测试为概率性守卫（窗口纳秒级，多数轮次走平凡路径），
注释中明确记录：加 spin-barrier"对齐"线程反而系统性避开窗口（实测）。
真正的正确性保证是锁融合论证，不是测试。

TODO：`todos/mpsc-permit-based-writers.md`（写侧升级为
permit/semaphore 无锁实现，含 reserve() 语义）。

## 排查扩展：broadcast / watch / oneshot（2026-08-24）

问题"其他 channel 是否也有此问题"的排查结论：

- **watch / broadcast**：等待全部委托 `Notify`（`changed()`/`recv()` 均为
  "条件检查 → co_await notified() → 醒来重查"循环），notify 修复后闭环，无需单独修复。
  TSan 0 警告。
- **oneshot**：无 check-then-act（resolve-once 事件模型的正确用法），**但 TSan 暴露了
  `ResolveState` 核心的真数据竞争**：`resolve()` 先 CAS `resolved=true` 再写载荷
  （promise_adapter.h 原 :125-126），而并发 `poll_state` 的 acquire 只与 CAS 同步、
  不覆盖其后的载荷写 → 并发 poll 在 CAS 与写载荷之间读到 true 即与写竞争
  （MoveOnly 类型可撕裂）。被唤醒的路径经 wake→post→pop 链有 happens-before，
  竞态的是未唤醒、恰好并发 poll 的路径——所有 async()/adapt()/timer/work 共用此状态。

  **修复**：tri-state 发布协议——`Waiting →(CAS 认领)→ Resolving →(写载荷后
  release-store)→ Ready`；poll 仅在 acquire-load 到 Ready 时读载荷
  （`promise_adapter.h`）。oneshot TSan 3 警告 → 0；全量 80/80 通过。

至此 sync 模块（mpsc/notify/watch/broadcast/oneshot）+ promise 核心（ResolveState）
在 TSan 下全部清零。
