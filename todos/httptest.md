# TODO: httptest 化 —— 基于 xpp::http::Server 重写 test_server.h

> 来源：test_server.h（540 行手写 HTTP fixture）与 Server 的重复；目标是对齐
> Go `net/http/httptest` 的人体工学，xpp 化
> 前置：#84（Response::ok）已合并 ✓；Router as Handler 已合并 ✓
> （`Server::builder().router(r)` 已存在，单 handler 构造即
> "整个 Router 是一个 handler"；原计划的 libx mux catch-all 前置作废）

## 目标

像 Go httptest 那样：**一行起一个真 HTTP 服务，handler 任意定制**，测试完自动收。

```go
// Go 的样子
ts := httptest.NewServer(http.HandlerFunc(func(w, r) { fmt.Fprintln(w, "hello") }))
defer ts.Close()
client.Get(ts.URL)
```

```cpp
// xpp 的样子 —— 构造即启动，RAII 收尾
xpp::http::test::Server ts([](xpp::http::Request req) {
  return xpp::http::Response::ok("hello");
});
// ts.url() == "http://127.0.0.1:54321"，析构自动 stop+drain
auto r = client.get(ts.url().c_str()).await();

// 多路由测试：主构造器收 builder 配置器（完整路由能力，含 :param 注入）
xpp::http::test::Server routed([](xpp::http::ServerBuilder &b) {
  b.route("GET /users/:id", [](Request req, String id) { return Response::ok(id); })
   .route("POST /echo", [](Request req) -> Promise<Result<Response>> {
       auto body = co_await req.into_body().bytes();
       return Response::ok(body.unwrap());
   });
});
```

## Go httptest 特性映射

| Go httptest | xpp 化 | 说明 |
| --- | --- | --- |
| `NewServer(h)` | `test::Server ts(handler)` —— **构造即启动** | 对齐 Go：`NewServer` 监听失败直接 panic；fixture 对 bind 失败 `XPP_ASSERT`（环境已坏，测试无法继续），不传播 `Result` |
| `NewServer(mux)`（Go 的 mux 本身是 handler） | `test::Server ts([](ServerBuilder &b){ b.route(...) ... })` | **主构造器**：收 builder 配置器，完整路由（`:param` 注入、多路由）；对应 Go 里"传整个 mux"的用法 |
| `ts.URL` | `ts.url()` → `"http://127.0.0.1:<port>"` | 现在测试手拼 `url_for(port, path)` |
| `ts.Close()` / defer | RAII dtor（stop + drain running） | 现有 TestServer 已有 |
| `NewTLSServer` + `ts.Client()` | **非目标**（暂缓） | libx xHttpServer 尚无 TLS serving；Client 是 libcurl 无信任配置需求 |
| `httptest.NewRequest` | 免费已有：`Request::builder()` | 纯值构造，无需网络 |
| `ResponseRecorder`（无网络测 handler） | 免费已有：直接调用 handler | 我们的 handler 返回 `Response` 值（无 io.Writer 边信道），`auto resp = h(req)` 即断言 |
| —（Go 没有） | `test::Server ts(spec)` | 保留数据驱动构造重载，echo/redirect/delay 一行配置 |

## API 设计

### 1. `xpp::http::test::Server`（替代现 TestServer，文件仍为 `test_server.h`）

```cpp
namespace xpp { namespace http { namespace test {

class Server {
public:
  // ── 主构造器：builder 配置器（完整路由能力）──
  // 构造即启动：套上 bind("127.0.0.1", 0) + build + serve，失败 XPP_ASSERT。
  // Go 的 NewServer(mux) 对应物 —— Go 的 mux 是 handler，我们的路由在
  // ServerBuilder 上，所以通用形态是"给你 builder，随便配"。
  template <class F> explicit Server(F &&configure);   // F: (ServerBuilder&) -> void

  // ── 便捷构造器：单 handler 接管一切（catch-all/fallback 路由）──
  // 适合静态/回显类简单 fixture；等价于 configure 里注册一个 fallback。
  template <class H> explicit Server(H &&handler);

  // ── 数据驱动重载：现有 TestResponseSpec 语义照搬 ──
  explicit Server(TestResponseSpec spec);

  Server(Server &&) noexcept;             // move-only
  ~Server();                              // RAII：stop() + running.await()

  String   url() const;                   // "http://127.0.0.1:<port>"
  uint16_t port() const;
  void     close();                       // 幂等；dtor 亦调用
};

}}} // namespace
```

### 2. 前置：Server 的 catch-all 路由（libx mux 层）

现状：`xHttpRouteMatch_` 只支持段精确 / `:param`，无通配——`Server(handler)` 无法一行接管。
**方案**：libx mux 支持保留 pattern `"*"`（任意 method + 任意 path，注册即 fallback；
resolve 循环里普通路由不中再试 `*` 路由）。xpp 侧暴露为
`ServerBuilder::fallback(h)`——顺带成为通用能力（自定义 404/默认 handler），
与 Go `http.NotFoundHandler` / mux `/` 前缀对齐。

### 3. `TestResponseSpec` 的实现迁移（spec → handler 编译）

| spec 字段 | Server 上的实现 | 迁移后 |
| --- | --- | --- |
| status/headers/body | sync handler 返回 preset Response | ✅ |
| `echo_request_body` | 协程 handler：`co_await req.into_body().bytes()` 回显 | ✅ |
| `echo_request_method` | handler 读 `req.method()` 写 `X-Echo-Method` | ✅ |
| `redirect_to` | handler：path ≠ target → 302 + Location | ✅ |
| `delay_ms` | 协程 handler：`co_await xpp::after(ms)` 再响应 | ✅ |
| `mid_body_delay_ms` | channel body + 延迟生产者：发前半 → `co_await after()` → 发后半。暂停生产=暂停发送，现 64KB 发送上限的 workaround 不再需要（server 只写已生产的数据） | ✅ |
| `truncate_body_after` | **不可表达**——需要"说谎的对端"（声明完整 Content-Length、只发一半、硬断连），well-formed Server 永远不会 | ❌ → EvilServer |

### 4. `xpp::http::test::EvilServer`（单独封装，新文件 `evil_server.h`）

fault-injection 对端，只做 well-formed 服务器做不到的事。首期仅保留截断注入：

```cpp
struct EvilSpec {
  Bytes   body;                  // 完整响应体（用于声明 Content-Length）
  size_t  send_only = 0;         // 实际只发前 N 字节然后硬断连
};

class EvilServer {               // 原始 TCP（从现 test_server.h 的 socket
public:                          // 路径精简而来，~80 行），构造即启动
  explicit EvilServer(EvilSpec spec);
  uint16_t port() const;
  void     close();              // RAII 同上
};
```

现 `client_test.cpp` 的 `TruncatedBodySurfacesAsError`（1MB 声明 / 64KB 实发）
迁移至此。后续如需更多恶意行为（乱序、坏 chunked、慢攻击）按需加字段。

### 5. 删除清单（重写后）

- 手写请求解析：`find_header_end` / `parse_content_length` / `parse_request_path` /
  `parse_request_method`（Server 的 llhttp 接管）
- 手写响应构建：`build_response` / `to_reason_phrase` 表（Response/StatusCode 接管）
- 手写 framing 与 `Connection: close` 管理、`pump_send` 的 64KB 上限 workaround
- 估算：540 行 → ~150 行（TestServer 适配层）+ ~80 行（EvilServer）

## 迁移计划

1. libx mux `"*"` catch-all + `xHttpRouteMatch_`/`xHttpMuxResolve` 支持 + 单测
2. xpp `ServerBuilder::fallback(h)` + server_test 补 fallback 用例
3. `test::Server`（handler + spec 双 start）+ `url()`
4. `test::EvilServer`（从现 socket 路径抽取精简）
5. `client_test.cpp` / `http_convenience_test.cpp` 全量迁移
   （truncate 测试 → EvilServer，其余 → 新 Server；`url_for` 换 `ts.url()`）
6. 删除旧实现

## 非目标

- TLS test server（等 libx TLS serving；届时对齐 `NewTLSServer`/`ts.Client()`）
- `httptest.NewRequest` / `ResponseRecorder` 等价物（`Request::builder()` 与
  值语义 handler 调用已免费覆盖）
- proxy/hijack 注入（Go `httptest.NewServer` 的 `ts.Config` 钩子）——按需再加

## 验证

- client_test / http_convenience_test / server_test 全量
- CI 四配置 ASan + TSan lane（重点：Linux shared build —— 旧实现头注释提到的
  "fiber/xEventLoopRun 交互问题"指向 fiber，Server 是 waker-driven 无 fiber，
  理论上顾虑消失，需 CI 实证）
- timing 类测试（delay 200ms / mid-body 2000ms）在 CI 慢机器上复核阈值余量
