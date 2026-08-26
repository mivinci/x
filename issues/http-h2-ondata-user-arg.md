# Issue: HTTP/2 下 on_data 回调 arg 与 h1 不一致 —— per-request 状态不可见

> 状态：fixed（`proto_h2.c` on_data 的 arg 对齐 h1 的 `stream->user ?: route_info->arg`）
> 首次观测：2026-08-26，重写 `libx/bench/http/https_bench_server.cpp` 到新 server API
> 后用 curl 冒烟测试 POST 时

## 现象

HTTPS bench server 的 `POST /echo` 导致进程崩溃（curl `exit 56` connection reset）：

- `curl --http1.1 POST` → 正常返回 echo
- `curl POST`（默认 ALPN 协商到 **h2**）→ server 崩溃
- `curl -v` 确认：`ALPN: server accepted h2`

## 根因

h1 与 h2 的 `on_data` 回调 arg 解析**不一致**：

- `proto_h1.c` `on_body`（:113）：

```c
  void *arg = stream->user ? stream->user : stream->route_info->arg;
```

  优先用 per-request 的 `stream->user`（由 `xHttpCtxSetUser` 设置）。

- `proto_h2.c` `h2_on_data_chunk_recv_callback`（:148，修复前）：
  
```c
  int rc = stream->route_info->on_data((const char *)data, len, stream->route_info->arg);
```

  **直接传 route 级 `route_info->arg`**，完全忽略 `stream->user`。

标准用法（`on_request` 里 `xHttpCtxSetUser(ctx, per_request_state)`，`on_data` 里取回）
在 h1 下成立，在 h2 下 `on_data` 拿到的是 route 级 arg（通常是 NULL）——bench 的
`echo_on_data` 解引用 NULL → crash。任何依赖 per-request user data 的 h2 handler
都会踩中。`on_done` 无此问题（`conn_dispatch_request` 在 server.c，两个协议共享，
用 `stream->user ?: route_info->arg`）。

## 修复

`proto_h2.c` 的 `h2_on_data_chunk_recv_callback` 改为与 h1 相同的 arg 解析：

```c
void *arg = stream->user ? stream->user : stream->route_info->arg;
int   rc  = stream->route_info->on_data((const char *)data, len, arg);
```

## 验证

- 修复后 `curl -sk -X POST -d body https://127.0.0.1:8443/echo`（h2）正常 echo；
  `--http1.1` 也正常。
- 现有测试：h2 测试（`server_h2_test.cpp`）通过（原测试的 on_data 用 route 级
  arg 静态状态，未覆盖 per-request user data 路径，所以此前未暴露）。

## 复现

```bash
# 构建 https_bench_server（X_BUILD_BENCHMARKS=ON）并启动
# POST 用默认协商（h2）即崩
curl -sk -X POST -d hi https://127.0.0.1:8443/echo
```
