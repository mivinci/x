# TODO: mpsc 写侧升级为 permit（semaphore）式无锁等待

> 来源：`issues/mpsc-single-slot-waiter-race.md` 修复（2026-08-24）的后续演进
> 优先级：低——当前 mutex 方案正确且只在满载冷路径取锁；本项在出现下述"触发条件"时再做

## 动机

mpsc 修复将 bounded 通道写侧等待改为 mutex + 侵入式 FIFO，与 tokio 的
semaphore/permit 模型相比有两处差距：

1. **多核高竞争的满载通道**下 mutex 串行化生产者（无锁 intrusive 队列不串行化）
2. **唤醒语义是"重试提示"而非"容量预留权"**（permit），无法支持
   "先拿容量再生产值"的背压模式（生产昂贵时避免白白生产）

## 内容

- 新增无锁 semaphore 原语（permit 记账 + 侵入式 waiter 链表 + per-waiter
  状态机），对齐 tokio `sync::Semaphore` 核心语义（acquire/release/close/FIFO 公平）
- `mpsc::Sender::send()` 满载路径迁移到 permit 获取：唤醒 = permit 移交，
  醒来后 push 保证成功（消除"重试失败需重新排队"路径）
- 新增公共 API：`Sender::reserve()` / `try_reserve()` 返回可持有/转移的
  `SendPermit<T>`（跨 await 点持有容量权，`permit.send(v)` 无阻塞必成功）
- `Chan` 对外协议（`wake_one_writer`/`wake_all_writers`/节点侵入式登记形状）
  保持不变，仅替换内部实现；unbounded 不受影响

### 与我们 mutex 方案的对比（决策记录）

| | tokio semaphore | 当前 mutex FIFO |
| --- | --- | --- |
| 容量记账 | 独立 permit 计数（预留权，醒来必成功） | 内嵌 ring（重试提示） |
| 排队 | 无锁 intrusive 链表 + per-waiter 状态机（loom 验证） | mutex 互斥，不变量一行论证 |
| 快路径 | 无锁 | 无锁（同量级） |
| 额外能力 | reserve()/Permit 持有转移/add_permits(n) | 无 |

## 非目标

- 不迁移 `AtomicPromiseWaker` 读侧（已无锁）
- 不实现 `add_permits(n)` 批量释放与 `recv_many`（按需再加）
- 不引入 loom 模型检查（以交错压力测试 + 状态机论证替代）

## 触发条件（何时做）

- 多核高竞争满载通道成为实测瓶颈
- 需要 `reserve()` 语义（背压下避免无效生产）

## 正确性要求

- per-waiter 状态机需覆盖：入队/出队/自取消与并发交付的竞态、close 批量唤醒、
  节点析构撤销登记（ABA）
- 回归：现有 `MpscMtTest` 全量 + `StressLostWakeupRegression` +
  `MultipleSuspendedSenders` + TSan 清零
