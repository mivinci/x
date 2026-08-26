# Issue: 流式响应 body 在连接关闭后写已释放的 ctx —— heap-use-after-free

> 状态：fixed（`on_close` 回调 + `ConnLifetime` 引用计数信号）
> 首次观测：PR #87 的 CI（2026-08-26）——`ubuntu-latest / openssl / C++11`（ASan）与
> `Symbol visibility (shared build)` 两个 lane 同时失败

## 现象

`libxpp/xpp/http/client_test.cpp` 的 `ClientSendTest.ReadTimeoutTriggersError`：

- ASan lane：`ERROR: AddressSanitizer: heap-use-after-free`，读 `xHttpCtxWrite`
  （`libx/x/http/server.c:1005`），freed by `xHttpConnClose`（`server.c:650`）
- 无 ASan 的 shared-build lane：同测试直接 SegFault

两个 lane 是**同一个 bug**——一个被 ASan 捕获，一个裸奔。

## 根因

响应流式路径 `stream_channel_body`（`libxpp/xpp/http/server.h`）跨挂起点持有裸
`xHttpCtx*`，而它只受 `ServerLifetime` 保护。`ServerLifetime::destroyed` 只在
`Server::~Server` 置位，**覆盖不到连接级销毁**：

1. 测试服务器流式返回 1MB body，中途停顿 2s（`mid_body_delay_ms`）
2. 客户端 `read_timeout(1000)`，`send()` 收到 header 成功，但 `bytes()` 读一半超时
   → 客户端断开连接
3. 服务端 event loop 读到 `n==0` → `xHttpConnClose` → `xHttpStreamDestroy` 释放
   stream（`xHttpCtx` 内嵌在 stream 里，`stream->ctx.internal_ = stream`）
4. 但 spawn 出去的 handler 任务还挂起在 `stream_channel_body` 的 body channel read 上；
   测试服务器稍后推下一个 chunk 时，continuation 拿着悬空的 `xHttpCtx*` 调
   `xHttpCtxWrite` → UAF

关键点：`xHttpConnClose` 路径**不会**触发 `on_done`（`srv_on_done_cb` 只在
`conn_dispatch_request` 的正常完成路径调用），所以既有的 per-request 清理钩子
完全感知不到连接异常关闭。

## 修复

三处小改动：

1. **C API**（`libx/x/http/server.h`）：`xHttpRouteInfo` 新增 `on_close` 回调
   （`xHttpCloseFunc`），语义是「stream 销毁前调用，覆盖所有 teardown 路径」。
2. **C 实现**（`libx/x/http/server.c`）：`xHttpConnClose` 在 `xHttpStreamDestroy`
   之前调用 `on_close`，arg 用 `route_info->arg`（**不是** `stream->user`，后者可能
   已被先前的 `on_done` 释放）。
3. **C++ 层**（`libxpp/xpp/http/server.h`）：引入 `ConnLifetime`（`Arc` + `bool
   closed`），存于 `ServerImpl::m_conns`（`Arc<ConnMap>`，key = ctx 地址）。`on_close`
   回调置 `closed=true`；spawn 任务与 `stream_channel_body` 持有该 Arc，每次碰 ctx
   前先查 `closed`。

为什么不给 stream 加引用计数：`xHttpCtxWrite` 不只访问 stream，还访问 `stream->conn`
→ 引用计数必须同时延长 conn（socket/TLS/协议状态）的生命周期，把「连接关闭」的
资源释放时序和「响应流写完」耦合，复杂且易泄漏。`on_close` 信号方案保持资源释放
时序不变，只多一个通知，与既有 `ServerLifetime` 模式同构。

## 验证

- `http_client_test` 13/13 通过（ASan 构建，含 `ReadTimeoutTriggersError`）
- `http_server_test` 16/16 通过
- `xhttp_test`（C 层）173/173 通过

## 复现

```bash
cmake -B build -G Ninja -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
cmake --build build --target http_client_test
./build/libxpp/xpp/http_client_test --gtest_filter=ClientSendTest.ReadTimeoutTriggersError
```

修复前：ASan 报 heap-use-after-free（`xHttpCtxWrite` → `stream_channel_body` lambda）。
