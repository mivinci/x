# Issue: Command.NonZeroExitCode 偶发失败 —— 子进程立即退出路径的 flaky 测试

> 状态：open（未修复；观测为 CI 偶发，本地 30 次压力未复现）
> 首次观测：PR #82 的 Symbol visibility lane（2026-08-25，run 2）——`sh -c "exit 42"` 的完成回调 10s 内未触发（`ctx.done == 0`、`exit_code == 0`）

## 现象

`libx/x/base/command_test.cpp` 的 `Command.NonZeroExitCode`（及同类立即退出命令测试）在 CI 上偶发失败：

```
command_test.cpp:188: Failure
Expected equality: ctx.done (0) vs 1
command_test.cpp:189: Failure
Expected equality: ctx.result.exit_code (0) vs 42
```

done 回调在测试的 10 秒等待窗口内从未触发。

## 复现尝试

- Linux 容器（Debian trixie、shared 构建、当前分支含 inflight/offload 修复）：`--gtest_filter='Command.NonZeroExitCode'` 循环 **30 次零失败**
- macOS ASan 全量：多次通过

未复现 → 判定为**加载敏感的时序 flake**（GitHub runner 高负载时）。与 PR #82 的改动
（含 inflight 重设计）无关——command 路径不使用 offload/task。

## 嫌疑方向（给接手者）

`command_posix.c` 的"子进程在事件源注册前退出"竞态处理（:645-659 的 waitpid probe，
注释明言 "Register the event source BEFORE the waitpid probe below. Do a
non-blocking waitpid to catch this race"）。高负载下该窗口可能变宽：

1. 事件源注册与 waitpid probe 之间的通知丢失路径
2. done 队列投递后的唤醒在重负载下被延迟超过 10s（epoll 唤醒丢失/饥饿）
3. 信号驱动（SIGCHLD）路径的信号合并导致的丢通知

## 复现/验证

```bash
# 高并发压力（本地 Linux 容器）
for i in $(seq 1 100); do ./build-export/libx/x/base/xbase_test \
  --gtest_filter='Command.NonZeroExitCode' 2>&1 | grep -q Failure && echo "FAIL at $i"; done
# CI 失败日志：PR #82 run 32801039556 / job 97661741007（Symbol visibility lane）
```
