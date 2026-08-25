# Issue: libx C 层预存数据竞争 —— TSan lane 首次全量运行暴露（timer / xnet / xp2p）

> 状态：open（未修复；TSan CI lane 已限定 `-L xpp` 范围规避，见 `.github/workflows/ci.yml` tsan job）
> 来源：2026-08-24 TSan 首次接入（`scripts/test-mac.sh --tsan` / CI tsan lane）后的全量运行
> 关联：`issues/mpsc-single-slot-waiter-race.md`（同日已修：xpp sync 模块 + promise 核心 + Enter/Leave 嵌套链，全部清零）

## 背景

ASan 与 TSan 运行时不兼容，只能独立构建/lane。TSan 接入后全量运行
（`zsh scripts/test-mac.sh -t openssl -B build-tsan --tsan`），除已修复的
xpp 层问题外，libx C 层暴露以下**预存**竞争（均非本次改动引入）：

## 已随本过程修复（记录）

- **Enter/Leave 嵌套链**：`l->prev` 存在共享 loop 结构体中，worker 线程
  Enter 同一 loop 时互覆写（TSan 竞态 + 恢复错误嵌套的真实 bug）。
  已改为 TLS 栈（`event_run.c`）。
  **二次修复（重要教训）**：TLS 栈第一版把"进入后的 loop"入栈，Leave 弹出时
  把 `tl_loop` 恢复成刚离开的 loop 本身而非进入前的绑定——每个 WaitScope 测试
  结束后 TLS 残留**已析构 loop 的指针**，后续无 WaitScope 的代码（如
  `TimerTest.ConstructOutsideWaitScope`）把定时器压进已释放 loop 的堆 →
  realloc 垃圾指针 → TSan 运行时 CHECK 崩溃。正确语义：栈保存**进入前**的
  绑定（对应旧 `l->prev` 存 tl_loop 旧值），Leave 恢复之。
- **符号误导教训**：TSan 崩溃栈在无 `-g` 的 dylib 上会用最近导出符号**猜**
  帧名（static 函数如 `submit_timer` 不在导出表）——曾据此得出错误定位。
  脚本 `--tsan` 已加 `-g`；诊断一律用带符号构建。
- `mpsc.c` 发布点、`done_head`/`loop_alive` 读、`list.h` Node::next——
  见 mpsc issue 修复记录。

## 未修复清单

### 1. `loop->stopped` 普通字段（小修）

`xEventLoopStop`（任意线程，event_run.c:267 写）vs `xEventLoopRun`
（loop 线程，:233/:238/:255 读 + :256 复位）。原子化（xAtomicLoad/Store）
即可，注意 Run 消费后复位的语义保留。
注：无符号构建时期归到 "timer 内部" 的部分警告实为此项（`xEventLoopStop`
vs `Run`），修复此项预计可清 xbase 大部分剩余警告。

### 2. timer 跨线程 stop 的判断缺陷（设计级）

`xTimerStop` 用 `xEventLoopCurrent() != timer->loop`（**TLS 绑定**）判断
能否走直接路径（event_timer.c:73）。worker 线程 `xEventLoopEnter(loop)` 后
该检查通过 → 在非 loop 线程执行 `timer_stop_direct`：读 `timer->fired`/
`heap_idx`（与 loop 线程的 timer 回调竞争，TSan 可见）且**跨线程操作
timer 堆**——原子化字段不解决堆操作，需要"loop 属主线程"概念
（loop->owner tid 由 Run 设置）或强制 post 路径。
复现：`BuiltinTimerConcurrent_CrossThreadStopStress`（xbase_test）。

### 3. xnet：14 个警告（未排查）

`./build-tsan/libx/x/net/xnet_test` 全量跑可见。预计 TCP/TLS/DNS 内部
同类问题（普通字段跨线程 + 事件回调路径），需逐个排查。

### 4. xp2p：2 个警告（未排查）

`./build-tsan/libx/x/p2p/xp2p_test`。ICE/DTLS/SCTP 栈，未排查。

### 5. allocator_test 与 TSan 分配器（非竞态，已处理）

测试故意请求超大分配，TSan 分配器上限低于 ASan → `allocation-size-too-big`
abort。已在脚本 `TSAN_OPTIONS` 加 `allocator_may_return_null=1`
（超大分配返回 null，测试按分配失败路径处理）。

## 修复路径建议

1. 先做 #1（原子化 stopped，xbase 里 4/5 个警告随之消失）
2. #2 需要设计决策（owner-thread 概念 vs 一律 post），单独评估
3. #3/#4 用 `zsh scripts/test-mac.sh -t openssl -B build-tsan --tsan` +
   逐模块跑（构建已支持），每清零一个模块把 TSan lane 范围扩大
4. 全部清零后移除 CI tsan job 的 `-L xpp` 过滤

## 复现

```bash
zsh scripts/test-mac.sh -t openssl -j $(sysctl -n hw.ncpu) -B build-tsan --tsan   # 全量
cd build-tsan && ctest -L xpp --output-on-failure                                  # 当前 lane 范围（绿）
./libx/x/base/xbase_test 2>&1 | grep -A 12 "WARNING: ThreadSanitizer"              # #1/#2
./libx/x/net/xnet_test 2>&1 | grep -A 12 "WARNING: ThreadSanitizer"               # #3
```

## CI 记录（2026-08-25）

PR #82 的 Linux TSan lane（gcc-12）出现 3 个失败（`promise_adapter_work_test`、
`sync_notify_test`、`sync_watch_test`），而 macOS/clang TSan 全绿。三份报告同型：

```
current:  operator delete(void*, size, std::align_val_t)   ← sized-aligned delete
previous: Arc 引用计数 fetch_sub（原子 RMW）
```

**诊断过程（含一次误判，留档）**：最初判断为 gcc-12 libtsan 的误报并把 lane
切到 clang——结果 **clang-19/Linux 稳定复现同样 3 个失败**（本地 Debian trixie
容器 + llvm-symbolizer 拿到完整符号栈）。真实根因：

`arc.h` 的引用计数递减用的是 boost::shared_ptr 的
"**release 递减 + 归零路径 acquire fence**"模式。这在 C++ 内存模型下合法
（RMW 延续 release sequence），但 **TSan 不建模 fence 穿过 RMW 链**——
这是 TSan 最著名的假阳性模式（libstdc++ shared_ptr 同款）。
macOS 未触发只是交错时机差异。

**修复（libc++ 的做法）**：`arc_dec_strong` / `arc_dec_weak_and_maybe_dealloc`
的 `fetch_sub` 改为 `memory_order_acq_rel`，删除 acquire fence——RMW 的
acquire 侧直接与前一 release 配对，TSan 正确建模；x86 上 LOCK RMW 本就全序，
ARM 上单指令差别。验证：Linux/clang-19 TSan `-L xpp` **69/69 全绿 0 警告**。

**教训**：跨平台 TSan 结果不一致时，先怀疑交错覆盖差异而非编译器实现差异；
本地复现环境（Apple container + Debian trixie + clang-19 + llvm-symbolizer）
是判定真伪的 fastest path。

其他 CI 修复：allocator.h 改用 unsized aligned delete（clang+libstdc++ 不默认
声明 sized-aligned 形式）；fiber.c 补 `_DEFAULT_SOURCE`（`_XOPEN_SOURCE`
单定义会隐藏 `MAP_ANONYMOUS`）。
