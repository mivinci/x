# Event Loop API

`event.h` 提供跨平台的事件循环抽象，底层在 macOS/BSD 使用 kqueue，Linux 使用 epoll，其他 POSIX 系统使用 poll，Windows 使用 WSAPoll，对外暴露统一的接口。

所有底层都以边缘触发（edge-triggered）模式运行，调用者需要一次性排空 fd 后再等待下一次通知。

## 事件循环

创建、启动和管理事件循环生命周期。

| API | 说明 |
|---|---|
| `xEventLoopCreate()` | 创建一个事件循环实例 |
| `xEventLoopCreateWithGroup(group)` | 创建事件循环并关联默认线程池（xWorkSubmit 的 group=NULL 时使用）|
| `xEventLoopDestroy(loop)` | 销毁事件循环，自动移除所有已注册的 IO 源 |
| `xEventLoopGlobal()` | 获取进程级全局事件循环，首次调用时创建，atexit 自动销毁 |

### 线程上下文

事件循环与线程绑定，通过 TLS（thread-local storage）跟踪当前线程的事件循环。

| API | 说明 |
|---|---|
| `xEventLoopEnter(loop)` | 将 loop 注册为当前线程的事件循环；返回之前注册的 loop（可为 NULL），便于嵌套和恢复 |
| `xEventLoopLeave()` | 注销当前线程的事件循环，等价于 `xEventLoopEnter(NULL)` |
| `xEventLoopCurrent()` | 获取当前线程注册的事件循环，未注册时返回 NULL |

**典型用法** — 在 IO / 定时器 / 回调中不需要显式 Enter/Leave，但在从其他线程调用事件循环 API 时需要：

```c
// 从另一线程提交任务
void submit_from_other_thread(xEventLoop loop, ...) {
    xEventLoopEnter(loop);
    xWorkSubmit(NULL, work_fn, done_fn, arg);
    xEventLoopLeave();
}
```

### 运行模式

与 libuv 对齐，提供三种运行模式：

| API | 说明 |
|---|---|
| `xEventLoopRun(loop, mode)` | 统一的事件循环入口 |
| `X_RUN_DEFAULT` | 阻塞运行，直到调用了 xEventLoopStop 或所有活跃句柄已关闭 |
| `X_RUN_ONCE` | 单次迭代，poll 阻塞到下一个定时器触发 |
| `X_RUN_NOWAIT` | 单次迭代，非阻塞 poll（无事件立即返回）|

### 停止与唤醒

| API | 说明 |
|---|---|
| `xEventLoopStop(loop)` | 设置停止标志并唤醒循环，线程安全，可从信号处理函数调用 |
| `xEventLoopWake(loop)` | 唤醒阻塞中的事件循环，多次唤醒合并为一次 |

```c
// xEventLoopRun 跑后台线程，主线程通过 xEventLoopStop 优雅退出
void *bg_thread(void *arg) {
    xEventLoopRun(loop, X_RUN_DEFAULT);
    return NULL;
}
void shutdown() {
    xEventLoopStop(loop);
    pthread_join(bg, NULL);
    xEventLoopDestroy(loop);
}
```

### 外部嵌入

| API | 说明 |
|---|---|
| `xEventLoopFd(loop)` | 返回底层轮询 fd（kqueue/epoll fd），poll/WSAPoll 返回 -1 |
| `xEventLoopNextTimeout(loop)` | 返回距离下次定时器触发的毫秒数，无定时器时返回 -1 |

配合 `X_RUN_NOWAIT` 将 libx 事件循环嵌入外部 run loop：

```c
int  fd      = xEventLoopFd(loop);
int  timeout = xEventLoopNextTimeout(loop);
host_poll_add(fd, ^{
    xEventLoopRun(loop, X_RUN_NOWAIT);
});
```

### 回调投递

| API | 说明 |
|---|---|
| `xEventLoopPost(loop, fn, arg)` | 将回调投递到指定事件循环的 done queue |

`xEventLoopPost` 将回调直接排入指定事件循环的 done queue，在下一次该事件循环的迭代时执行。回调在事件循环线程中执行，因此不能阻塞。

线程安全，可直接从其他线程给任意事件循环投递：

```c
void notify_from_other_thread(xEventLoop loop) {
    xEventLoopPost(loop, on_notify, ctx);
}
```

与 `xWorkSubmit` 的区别：`xEventLoopPost` 不涉及线程池，回调直接在事件循环线程执行；`xWorkSubmit` 将阻塞任务提交到线程池，完成后通过 done_fn 回调。

### 设计原理

事件循环内部维护引用计数（`loop->active_handles`），跟踪所有活跃的 IO 句柄和定时器。`loop_alive()` 检查引用计数、定时器堆、done queue 和 inflight 任务，当所有任务完成后 `xEventLoopRun` 自动退出。

每次 `xEventLoopRun` 迭代的处理流程（对齐 libuv 的 `uv_run`）：
1. `done_queue` — 排空完成队列（offload 结果 + post 回调）
2. `poll` — 阻塞等待 IO 事件（根据 `can_sleep` 和定时器超时决定是否阻塞）
3. `done_queue` — 再次排空（post-poll 完成回调）
4. `timers` — 触发到期的定时器
5. `sweep` — 清理已删除的 IO 源

`loop->time` 缓存最新时间戳，通过 `loop_update_time()` 在每个迭代周期更新一次，避免频繁系统调用。

## IO 任务

监听文件描述符的读写就绪事件，在事件循环线程中触发回调。

| API | 说明 |
|---|---|
| `xEventAdd(fd, mask, fn, arg)` | 注册 fd，监听指定事件（xEvent_Read / xEvent_Write）|
| `xEventMod(src, mask)` | 修改已注册 fd 的监听事件 |
| `xEventDel(src)` | 移除 fd 的监听，不关闭 fd |

```c
// 监听 stdin
xEventSource src = xEventAdd(STDIN_FILENO, xEvent_Read, on_stdin, NULL);

while (running) {
    xEventLoopRun(loop, X_RUN_ONCE);
}

xEventDel(src);
```

**边缘触发注意事项**：底层以 ET 模式运行，回调中必须一次性读取直到返回 EAGAIN，否则不会再次收到通知。

```c
void on_stdin(int fd, xEventMask mask, void *arg) {
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0)  process(buf, n);
        if (n == 0) { xEventDel(src); close(fd); break; }
        if (n < 0 && errno == EAGAIN) break;
        if (n < 0 && errno != EINTR) { /* error */ break; }
    }
}
```

## 定时任务

基于事件循环内的定时器堆（最小堆），在事件循环线程中触发回调。

| API | 说明 |
|---|---|
| `xTimerStart(fn, arg, NULL, timeout_ms, repeat_ms)` | 创建定时器；repeat_ms=0 表示单次触发，>0 表示重复触发 |
| `xTimerStop(timer)` | 停止定时器，线程安全（从其他线程调用时内部通过 xEventLoopPost 投递）|

```c
// 100ms 后一次性超时
xTimerStart(on_timeout, ctx, NULL, 100, 0);

// 每 50ms 重复触发
xTimer t = xTimerStart(on_tick, ctx, NULL, 50, 50);

// 从其他线程停止
xTimerStop(t);
```

**实现要点**：
- 定时器基于 `xMonoMs()` 的单调时钟，不依赖系统日期变化
- 定时器堆在单线程环境下操作，无锁开销
- `xTimerStop` 可从任意线程调用：在事件循环线程中直接移除；否则通过 `xEventLoopPost` 投递到事件循环线程执行
- 重复定时器在回调执行后才重新入堆，不会产生事件堆积

## 信号

将 POSIX 信号接入事件循环，回调在事件循环线程中执行（不在信号上下文中，安全调用任何 API）。

| API | 说明 |
|---|---|
| `xSignal(signo, fn, arg)` | 注册信号监听；fn=NULL 则取消监听，恢复 SIG_DFL |

```c
// 优雅处理 SIGINT
xSignal(SIGINT, [](int signo, void *arg) {
    xEventLoopStop((xEventLoop)arg);
}, loop);
xEventLoopRun(loop, X_RUN_DEFAULT);
```

信号处理在底层通过信号管道（epoll/poll）或原生 kqueue EVFILT_SIGNAL（kqueue）接入事件循环。

## 阻塞任务

将耗时阻塞操作（如 DNS 解析、文件 I/O、CPU 密集计算）提交到线程池，完成后在事件循环线程执行完成回调。

| API | 说明 |
|---|---|
| `xWorkSubmit(group, work_fn, done_fn, arg)` | 提交阻塞任务到线程池；返回 xWork 句柄（可用于取消）|
| `xWorkCancel(work)` | 取消尚未开始的任务，取消成功则 done_fn 不会调用 |

**work_fn 在线程池线程执行**（可以阻塞），**done_fn 在事件循环线程执行**（可以安全调用任何事件循环 API）。

```c
// 典型用法：DNS 解析 + 结果处理
void *do_dns(void *arg) {
    struct addrinfo *result = NULL;
    getaddrinfo(hostname, NULL, NULL, &result);
    return result;
}

void on_done(void *arg, void *result) {
    struct addrinfo *ai = (struct addrinfo *)result;
    xTcpConnect_from_addr(ai->ai_addr, ...);
    freeaddrinfo(ai);
}

xWork w = xWorkSubmit(NULL, do_dns, on_done, ctx);
```

### 任务取消

`xWorkCancel(work)` 在线程池任务尚未开始执行时取消：

- 成功 — done_fn 不会被调用，调用者可安全释放 arg
- 失败（返回 xErrno_Busy）— 任务已在执行或已完成，done_fn 仍会被正常调用
