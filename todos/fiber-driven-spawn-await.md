# TODO: fiber 驱动的 spawn —— 让 spawn fn 里的 `.await()` 普遍可用

> 来源：README 示例改 spawn 时发现（2026-08-26）——`spawn` + `.await()` 在
> 生产/消费依赖循环场景死锁
> 优先级：低——`co_await`/`.then()` 是 spawn 的惯用等待方式，已正确；
> 本项只服务「想用同步 `.await()` 写 spawn 逻辑」的场景

## 动机

`spawn` 的 fn 里用 `.await()` 时，`PromiseContext::park()` 走**同步阻塞 + 嵌套
X_RUN_ONCE**：当前执行流（spawn_step 的栈）park 时重入 loop。一旦两个 spawn 任务
互相依赖（consumer `recv().await()` 等 producer `send`，producer `send` 又等
consumer 消费以释放 channel 空间），形成**嵌套 park 环**：

```
consumer recv().await() → park [X_RUN_ONCE]
  → p1 send(9).await() → park [X_RUN_ONCE]
    → p2 send(11).await() → park [X_RUN_ONCE] → 无事件空转
```

最内层空转使外层 `X_RUN_ONCE` 永不返回 → consumer 的 recv waker 即使被置位也无法
重新 poll → 空间不释放 → send 永远等待。死锁。

根因：**同步 `.await()` 无法非阻塞挂起**（C++11 无栈恢复）。timer/I/O 场景安全
（回调不嵌套 park），跨 spawn 任务的依赖循环必死锁——已在 spawn.h 文档记录限制。

## 内容

让 `spawn(fn)` 的 fn 跑在 **fiber** 里（项目已有 `XPP_FIBER`/`xpp::fiber`）：

- `spawn` 创建 fn 时分配 fiber，在 fiber 栈上执行 fn
- fn 里的 `.await()` 的 park 自动走 fiber 路径 `xFiberYield()`（挂起 fiber、
  **不嵌套 loop**），waker 触发时 `xFiberSwitch` 恢复
- consumer 挂起时 loop 正常返回，p1 send → waker → switch 回 consumer → 消费 →
  空间释放 → p1 恢复 → 所有依赖循环推进，**彻底根治**

`PromiseContext::park()` 和 `PromiseWaker::create()` 已有 fiber 分支（fiber 上下文
自动绑定），基础都在。需要做的是：spawn 的 defer/spawn_step 与 fiber 调度的集成
（创建 fiber、切换、JoinHandle resolve 与 fiber 完成对齐、fiber 栈生命周期管理）。

### 约束

- 与现有 **poll 驱动 spawn** 保持语义一致（JoinHandle、waker re-post、跨线程
  WakeToken post 到 loop）
- `XPP_FIBER=OFF` 时回退到当前同步实现（并沿用文档限制）
- 不动 `co_await`/`.then()`（已正确，是 spawn 的惯用方式）

## 非目标

- 不改 `Promise::await()` 在 fiber 里的语义（已正确）
- 不引入协程改造现有同步 API
- 不保证 `.await()` 在 spawn fn 里等「本 fn 自己返回的 JoinHandle」（仍死锁，
  语义上就是循环等待）

## 触发条件

- 用户明确需要「用同步 `.await()` 顺序风格写 spawn 逻辑」且现有文档限制
  （依赖循环死锁）成为实际障碍
- 或 `xpp::fiber` 与 `spawn` 的语义统一成为设计目标
