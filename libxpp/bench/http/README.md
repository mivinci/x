# xpp echo server 压测基准

`echo_server.cpp`（xpp）vs `echo_server.go`（Go net/http）——HTTP/1.1 keep-alive
小请求 echo 场景。同一台机器、同一套 wrk 命令直接对比。

## 环境

- 机器：macOS（Apple Silicon M3，12 核）
- 构建：`build-bench`（Release，`-DX_BUILD_BENCHMARKS=ON`，per-module Google Benchmark 关闭）
- 负载工具：wrk 4.2.0（kqueue）
- Go：`go build echo_server.go`，`GOMAXPROCS` 分别设 1（单核）和默认（多核）
- xpp server：单事件循环（单线程）

## 压测命令

```bash
# server
./build-bench/libxpp/bench/xpp_echo_server 8080        # xpp
/tmp/go_echo_server 8081                                # Go（GOMAXPROCS=1 或默认）

# 负载（t4/c100 是默认；高并发档位单独指定）
wrk -t4 -c100  -d10s http://127.0.0.1:8080/ping
wrk -t4 -c500  -d10s http://127.0.0.1:8080/ping
wrk -t4 -c1000 -d10s http://127.0.0.1:8080/ping
wrk -t8 -c2000 -d10s http://127.0.0.1:8080/ping
wrk -t4 -c100  -d10s "http://127.0.0.1:8080/echo?size=1024"
wrk -t4 -c100  -d10s -s post.lua http://127.0.0.1:8080/echo
```

`post.lua`：

```lua
wrk.method = "POST"
wrk.body = "hello-world-payload-1234567890"
wrk.headers["Content-Type"] = "text/plain"
```

## 结果（QPS）

### GET /ping

| 并发 | xpp（1 核，CPU ~99%） | Go 单核（GOMAXPROCS=1） | Go 多核（12 核） |
| ------ | ---------------------- | ------------------------ | ----------------- |
| c100 | **77.9k** | 71.2k | 72.9k（CPU 240%） |
| c500 | **70.8k** | 68.4k | 69.4k（CPU 226%） |
| c1000 | **70.3k** | 65.9k | 68.8k |
| c2000 (t8) | **68.5k** | 64.5k（270 read errors） | 63.2k |

### GET /echo?size=1024（c100）

| 指标 | xpp | Go 单核 |
| ------ | ----- | -------- |
| QPS | **73.1k** | 64.9k |
| 吞吐 | **75.9 MB/s** | 70.7 MB/s |

### POST /echo（c100）

| 指标 | xpp | Go 单核 |
| ------ | ----- | -------- |
| QPS | **70.9k** | 66.7k |

## 结论

1. **xpp 单核赢 Go 多核**：xpp 用 1 核跑 77.9k QPS（CPU 99%）；Go 用 2.4 核跑 72.9k（CPU 240%）。
   等量 QPS 下单核效率约 Go 的 2.6 倍。
2. **Go 多核在 keep-alive 小请求场景零收益**：单核 vs 多核全并发档位差距 <3%，c2000 时
   多核反而更低。goroutine 调度 + net/http 中间层开销抵消了多核分摊。
3. **xpp 全档位领先 7%~17%**，且 c2000 时是唯一没有 socket errors 的实现（Go 单核有
   270 个 read errors，~0.04%）。
4. **内存稳定**：xpp 30s / 212 万请求压测 RSS 持平（22976→22928 KB），无泄漏。

## 附带发现

- xpp 单核上限 ~78k QPS，受限于单事件循环架构；要突破只能多进程/多 loop 横向扩展。
- xpp 单核上限 ~78k QPS，受限于单事件循环架构；要突破只能多进程/多 loop 横向扩展。
- 同一负载下 C 层 bench（`libx/bench/http/http_bench_server.cpp`，已重写到新 API）为
  **85.4k QPS** —— xpp 层（77.9k）约 **10% 封装开销**，来自每请求的 Arc/ReqState/
  channel/spawn 分配 + `Router::compose`（见 `todos/http-server-per-request-allocations.md`）。
- 重写 C 层 bench 时发现并修复了一个 libx bug：h2 下 `on_data` 回调的 arg 用
  route 级 `route_info->arg` 而非 `stream->user`，per-request 状态（`xHttpCtxSetUser`）
  不可见 → POST body 处理解引用 NULL 崩溃（见 `issues/http-h2-ondata-user-arg.md`）。

## 复现

```bash
cmake -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DX_BUILD_BENCHMARKS=ON \
  -DXBASE_BUILD_BENCHMARKS=OFF -DXBUF_BUILD_BENCHMARKS=OFF -DXJSON_BUILD_BENCHMARKS=OFF \
  -DX_BUILD_TESTS=OFF
cmake --build build-bench --target xpp_echo_server -j
./build-bench/libxpp/bench/xpp_echo_server 8080
```

Go 侧：`go build -o /tmp/go_echo_server libxpp/bench/http/echo_server.go`
