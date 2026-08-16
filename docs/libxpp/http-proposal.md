# libxpp HTTP 模块重新设计 Proposal

> 分支：`codebuddy/http-module-redesign`
> 状态：design phase
> 日期：2026-08-16

## 1. 背景

### 1.1 旧分支的问题

旧分支 `add-http-module`（2026-07-08 ~ 07-10，25 个 commit，1220 行代码 + 87 测试）有以下设计问题：

**1. Body 模型在 push/pull 之间反复横跳**

`client.h:132-137` 用 push（`on_data` 拷到 `std::deque<bytes::Bytes>`），`response.h:81-87` 用 pull（`body(R&&reader)` 接受 `TryRead`），`client.h:154-173` 在 `on_done` 里又把 deque 包装成 `read_fn`——两套机制叠加。最后一个 commit `217978b revert: request body pre-buffering` 就是这次反复的痕迹。

**2. Body 用 `std::function<ssize_t(char*, size_t)>` 抽象，绕过 io 模块**

`request.h:118`、`response.h:124` 用同步 `TryRead`（`try_read() → ssize_t`），跟 xpp 已有的 `AsyncReader` 概念（`read() → Promise<ssize_t>`）不兼容。`Response::text()`（`response.h:129-143`）写了一个**阻塞 while 循环**调 `fn(buf, 4096)`——这在单线程异步引擎里会卡死 EventLoop。

**3. HeaderMap 用 `std::multimap<std::string, std::string>`**

`header.h:28`——每次 `get()` 要 `lower(key)` 造一个临时 string，红黑树节点分散在 heap 上，cache 不友好。HTTP header 数量少（通常 5-20 个），线性扫描比红黑树快。

**4. 没有 `http::get(url)` 便捷函数**

Presentation 承诺 `xpp::http::get(url).await()`，实际要写 4 行 builder。

**5. Server handler 在 C 回调里 `.await()`**

`server.h:188`：`auto result = rs->handler(req).await();`——会 park EventLoop，handler 里再发异步请求会死锁。

**6. 重复发明轮子：`bytes::Bytes` 和 `bytes::Reader`**

分支自带 `libxpp/xpp/bytes/` 模块，但 xpp 主线已经有 `libxpp/xpp/io/`（含 `BufReader`、`AsyncReader` 概念、`read_all`、`copy`）——功能重叠。

### 1.2 重新设计的目标

对齐 Rust 生态的 hyper + reqwest：

- **hyper**：底层 HTTP 实现，`Body` 是 enum（Empty/Once/Channel），`Body::channel()` 用 mpsc 桥接 push→pull
- **reqwest**：高层 API，`Response::bytes()` / `text()` / `json()` 便捷方法，`Client` + `ClientBuilder`

我们的处境跟 hyper 0.10 → 0.12 重构一样：把 push 回调式的 C 库（libx + libcurl），用 mpsc channel 桥接到 pull 式的 async API。

## 2. 设计目标

| 目标 | 说明 |
|---|---|
| **Body 跟 io 模块同构** | `Body` 满足 `AsyncReader` 概念，能 `io::read_all(resp.body())`、`io::copy(reader, body)` |
| **零同步阻塞** | 所有 body 消费走 Promise 链，不在 C 回调里 `.await()` |
| **API 对齐 reqwest/hyper** | 类型名、方法名尽量一致，降低用户学习成本 |
| **`http::get(url)` 便捷函数** | 对齐 `reqwest::get`，一行发起请求 |
| **天然背压** | channel 满了 C 回调返回 -1，让 libx 暂停推送 |
| **不引入冗余模块** | 复用 io、sync、net；`Bytes` 放 `xpp::` 顶层 |
| **Server handler 返回 Promise** | 不在 C 回调里阻塞，handler 该 await 就 await |

## 3. 核心设计

### 3.1 Push→Pull 桥接

libx 的 C 回调是 push（`on_data(data, len)`），xpp 的 io 是 pull（`read(buf, len) → Promise<ssize_t>`）。中间用 **mpsc channel** 桥接：

```
C 回调端（push）                    Channel                   消费端（pull）
─────────────                       ─────────                 ─────────────
on_data(chunk)  ──→ try_send(chunk)  ──→ recv().await()  ──→ read() 返回
on_done()       ──→ close channel
```

**Channel 容量**：bounded 64 chunks。满了 `try_send` 返回 `Full`，C 回调返回 -1 让 libx 暂停——天然背压，不会 OOM。

### 3.2 `Bytes` 类型

新增 `xpp::Bytes`——**引用计数的不可变字节块**，对齐 Rust `bytes::Bytes`。

```cpp
class Bytes {
public:
  static Bytes empty();
  static Bytes from(Vec<uint8_t> vec);              // 接管 Vec
  static Bytes from(String s);                       // UTF-8 字节
  static Bytes from(const char* s);
  static Bytes from(const uint8_t* data, size_t len);
  static Bytes copy(const char* data, size_t len);

  // 拷贝便宜（refcount++）
  Bytes(const Bytes&)                = default;
  Bytes& operator=(const Bytes&)     = default;
  Bytes(Bytes&&) noexcept            = default;
  Bytes& operator=(Bytes&&) noexcept = default;

  // 零拷贝视图
  Span<const uint8_t> as_span() const;
  const uint8_t*      data() const;
  size_t              size() const;
  bool                empty() const;

  // 零拷贝切片
  Bytes slice(size_t offset, size_t len) const;
  Bytes slice_from(size_t offset) const;

  // 拷贝出来（要修改时用）
  Vec<uint8_t> to_vec() const;

  // UTF-8 解码
  Result<String> to_string() const;             // 失败返回 Err
  String         to_string_lossy() const;

  const uint8_t* begin() const;
  const uint8_t* end()   const;

private:
  struct Impl { Vec<uint8_t> buf; };
  Shared<Impl>  m_impl;            // Rc 或 Arc，编译期决定（-DXPP_MT）
  size_t        m_offset = 0;
  size_t        m_len    = 0;
};
```

**关键性质**：
- `sizeof(Bytes) = 24`（64 位）
- 拷贝 O(1) refcount++
- `slice()` O(1)，不拷贝数据
- `to_vec()` 才拷贝

**为什么不用 `Vec<uint8_t>`**：

| 操作 | `Vec<uint8_t>` | `Bytes` |
|---|---|---|
| copy | O(n) malloc + memcpy | O(1) refcount++ |
| slice | 必须拷贝或借裸指针（生命周期难管）| O(1)，安全 |
| 跨 Promise 传递 | move 后原对象空了 | copy 即可，多个 Promise 持有同一份 |
| Channel 传递 | 一次只能给一个 Receiver | 同上，但便宜 |

HTTP body chunk 经常要切片（`ChannelReader` 切半个 chunk 给用户，剩下半个下次返回），`Bytes` 让切片零拷贝。

**放哪里**：`libxpp/xpp/bytes.h`（单文件，不放 `bytes/` 目录），类型 `xpp::Bytes`——跟 `xpp::String`、`xpp::Vec` 并列。不放 `xpp::bytes::Bytes`——基础类型用得频繁，短名字更好。

### 3.3 `Body` 类型

对齐 hyper `Body`——enum，三种状态：

```cpp
class Body {
public:
  // 工厂
  static Body empty();
  static Body from(Bytes bytes);
  static Body from(Vec<uint8_t> bytes);     // → Bytes::from
  static Body from(String text);
  static Body from(const char* text);
  static Body from_channel(sync::mpsc::Receiver<Bytes> rx);

  Body() = default;  // = empty()
  Body(Body&&) noexcept           = default;
  Body& operator=(Body&&) noexcept = default;
  Body(const Body&)               = delete;
  Body& operator=(const Body&)     = delete;

  // AsyncReader 概念
  Promise<ssize_t> read(void* buf, size_t len);

  // 便捷消费
  Promise<Result<Bytes>>  bytes();   // = io::read_all(*this) 累积成 Bytes
  Promise<Result<String>> text();    // = bytes() + UTF-8 校验

  bool is_empty() const;
  bool is_channel() const;

private:
  enum class Kind { Empty, Once, Channel };
  Kind                         m_kind    = Kind::Empty;
  Bytes                        m_once;       // for Once
  sync::mpsc::Receiver<Bytes>  m_rx;         // for Channel
  Bytes                        m_pending;    // Channel 模式下未读完的切片
};
```

**`read()` 内部逻辑**：

```cpp
Promise<ssize_t> Body::read(void* buf, size_t len) {
  switch (m_kind) {
    case Kind::Empty:
      co_return 0;  // EOF

    case Kind::Once: {
      // 从 m_once 拷贝到 buf，切片剩余
      size_t n = std::min(len, m_once.size());
      std::memcpy(buf, m_once.data(), n);
      m_once = m_once.slice_from(n);  // 零拷贝切片
      if (m_once.empty()) m_kind = Kind::Empty;
      co_return n;
    }

    case Kind::Channel: {
      // 先用 m_pending
      if (!m_pending.empty()) {
        size_t n = std::min(len, m_pending.size());
        std::memcpy(buf, m_pending.data(), n);
        m_pending = m_pending.slice_from(n);
        co_return n;
      }
      // 从 channel 收一个 chunk
      auto chunk = co_await m_rx.recv();
      if (chunk.is_none()) co_return 0;  // channel 关闭 = EOF
      Bytes b = chunk.unwrap();
      size_t n = std::min(len, b.size());
      std::memcpy(buf, b.data(), n);
      if (n < b.size()) m_pending = b.slice_from(n);  // 剩下的存起来
      co_return n;
    }
  }
}
```

**`bytes()` 内部**：

```cpp
Promise<Result<Bytes>> Body::bytes() {
  // 用 io::read_all 累积所有 chunk
  // 失败（连接断开）返回 Err
}
```

### 3.4 Client 端流程

```cpp
Promise<Result<Response>> Client::send(Request req) {
  auto [tx, rx] = sync::mpsc::channel<Bytes>(64);
  auto [p, resolver] = async<Result<Response>>();

  // SendAdapter 持有 tx 和 resolver
  auto* adapter = new SendAdapter(std::move(resolver), std::move(tx));

  // 配置 xHttpRequestConf 的回调
  xHttpClientDo(m_client, &conf, adapter);

  return std::move(p);
}

// C 回调端
static int on_response(xHttpCtx* ctx, void* arg) {
  auto* self = static_cast<SendAdapter*>(arg);
  ResponseBuilder b;
  b.status(ctx->status_code);
  // parse headers...
  b.body(Body::from_channel(self->m_rx));  // body 走 channel
  self->m_resolver.resolve(Ok(b.build()));
  return 0;
}

static int on_data(const char* data, size_t len, void* arg) {
  auto* self = static_cast<SendAdapter*>(arg);
  auto r = self->m_tx.try_send(Bytes::copy(data, len));
  if (r.is_err() && r.unwrap_err().kind == TrySendError::Full) {
    return -1;  // 背压：让 libx 暂停
  }
  return 0;
}

static void on_done(xHttpCtx* ctx, void* arg) {
  auto* self = static_cast<SendAdapter*>(arg);
  self->m_tx.close();  // 让 Body::read() 返回 0 (EOF)
  delete self;
}
```

## 4. Client 侧 API 完整清单

### 4.1 顶层便捷函数（`xpp::http::get/post/...`）

对齐 `reqwest::get` 等——每次新建一个默认 Client。

```cpp
namespace xpp::http {

// GET（无 body）
Promise<Result<Response>> get(String url);
Promise<Result<Response>> get(const char* url);
Promise<Result<Response>> get(std::string_view url);

// POST（可选 body）
Promise<Result<Response>> post(String url);
Promise<Result<Response>> post(const char* url);
Promise<Result<Response>> post(std::string_view url);
Promise<Result<Response>> post(String url, Body body);
Promise<Result<Response>> post(const char* url, Body body);
Promise<Result<Response>> post(std::string_view url, Body body);

// PUT（同 POST 模式）
Promise<Result<Response>> put(String url);
Promise<Result<Response>> put(const char* url);
Promise<Result<Response>> put(std::string_view url);
Promise<Result<Response>> put(String url, Body body);
Promise<Result<Response>> put(const char* url, Body body);
Promise<Result<Response>> put(std::string_view url, Body body);

// DELETE（无 body）
Promise<Result<Response>> delete_(String url);
Promise<Result<Response>> delete_(const char* url);
Promise<Result<Response>> delete_(std::string_view url);

// PATCH（同 POST 模式）
Promise<Result<Response>> patch(String url);
Promise<Result<Response>> patch(const char* url);
Promise<Result<Response>> patch(std::string_view url);
Promise<Result<Response>> patch(String url, Body body);
Promise<Result<Response>> patch(const char* url, Body body);
Promise<Result<Response>> patch(std::string_view url, Body body);

// HEAD（无 body）
Promise<Result<Response>> head(String url);
Promise<Result<Response>> head(const char* url);
Promise<Result<Response>> head(std::string_view url);

}  // namespace xpp::http
```

### 4.2 `Client`

```cpp
class Client {
public:
  static ClientBuilder builder();

  Client(Client&&) noexcept            = default;
  Client& operator=(Client&&) noexcept = default;
  Client(const Client&)                = delete;
  Client& operator=(const Client&)     = delete;

  // 通用入口（对齐 reqwest::Client::send）
  Promise<Result<Response>> send(Request req);

  // 便捷方法（对齐 reqwest::Client::get/post/...）
  Promise<Result<Response>> get(String url);
  Promise<Result<Response>> get(const char* url);
  Promise<Result<Response>> get(std::string_view url);

  Promise<Result<Response>> post(String url);
  Promise<Result<Response>> post(const char* url);
  Promise<Result<Response>> post(std::string_view url);
  Promise<Result<Response>> post(String url, Body body);
  Promise<Result<Response>> post(const char* url, Body body);
  Promise<Result<Response>> post(std::string_view url, Body body);

  // put / patch 同 POST 模式
  // delete_ / head 只有 url 重载，无 body

private:
  friend class ClientBuilder;
  Client() = default;
  struct Deleter;
  OwnedHandle<Deleter> m_client;
};
```

### 4.3 `ClientBuilder`

```cpp
class ClientBuilder {
public:
  // 超时
  ClientBuilder& timeout(Duration d);            // 总超时
  ClientBuilder& connect_timeout(Duration d);
  ClientBuilder& read_timeout(Duration d);

  // 默认 header（所有请求）
  ClientBuilder& header(String key, String value);
  ClientBuilder& user_agent(String ua);           // 等价 header("User-Agent", ua)

  // 重定向
  ClientBuilder& redirect(RedirectPolicy policy);
  ClientBuilder& max_redirects(size_t n);

  // 代理
  ClientBuilder& proxy(String url);
  ClientBuilder& no_proxy();

  // TLS
  ClientBuilder& tls(TlsConfig config);
  ClientBuilder& danger_accept_invalid_certs(bool v);

  // HTTP 版本
  ClientBuilder& http1_only();
  ClientBuilder& http2_prior_knowledge();

  // 认证便捷
  ClientBuilder& bearer_auth(String token);
  ClientBuilder& basic_auth(String user, String pass);

  // 终结
  Result<Client> build();

private:
  friend class Client;
  ClientBuilder() = default;
  // ... 配置字段
};
```

### 4.4 `Request` + `RequestBuilder`

```cpp
class Request {
public:
  static RequestBuilder builder();

  Method            method()  const;
  const String&     url()     const;
  const HeaderMap&  headers() const;

  Body&             body();          // 借用
  Body              into_body();     // move out
  bool              has_body() const;

private:
  friend class RequestBuilder;
  Request() = default;
  Method     m_method = Method::Get;
  String     m_url;
  HeaderMap  m_headers;
  Body       m_body;
};

class RequestBuilder {
public:
  RequestBuilder& method(Method m);

  RequestBuilder& url(String u);
  RequestBuilder& url(const char* u);
  RequestBuilder& url(std::string_view u);

  RequestBuilder& header(String key, String value);
  RequestBuilder& header(String key, const char* value);

  RequestBuilder& bearer_auth(String token);
  RequestBuilder& basic_auth(String user, String password);

  // Body 终结
  Result<Request> body(Body body);
  Result<Request> body(Bytes bytes);
  Result<Request> body(Vec<uint8_t> bytes);
  Result<Request> body(String text);
  Result<Request> body(const char* text);
  Result<Request> body();                        // empty

  // 便捷终结
  Result<Request> get(String u);
  Result<Request> get(const char* u);
  Result<Request> post(String u);
  Result<Request> post(const char* u);
  Result<Request> post(String u, Body body);
  Result<Request> post(const char* u, Body body);
  // put / patch / delete_ / head 同样模式

private:
  friend class Request;
  RequestBuilder() = default;
  Method     m_method = Method::Get;
  String     m_url;
  HeaderMap  m_headers;
};
```

### 4.5 `Response` + `ResponseBuilder`

```cpp
class Response {
public:
  static ResponseBuilder builder();

  Response()                                = default;
  Response(Response&&) noexcept            = default;
  Response& operator=(Response&&) noexcept = default;

  // 状态行
  StatusCode        status()      const;
  uint16_t          status_code() const;

  // Headers
  const HeaderMap&  headers() const;
  Option<const String&> header(const String& name) const;

  // Body 访问
  Body&             body();          // 借用
  Body              into_body();     // move out
  bool              has_body() const;

  // 便捷消费（对齐 reqwest::Response::bytes / text）
  Promise<Result<Bytes>>  bytes();   // = into_body().bytes()
  Promise<Result<String>> text();    // = into_body().text()

  // 其他
  Option<const String&> url() const;     // 最终 URL（重定向后可能变）

private:
  friend class ResponseBuilder;
  StatusCode  m_status   = StatusCode::Ok;
  HeaderMap   m_headers;
  Body        m_body;
  String      m_final_url;
};

class ResponseBuilder {
public:
  ResponseBuilder& status(StatusCode code);
  ResponseBuilder& status(uint16_t code);

  ResponseBuilder& header(String key, String value);

  // Body 终结
  Response body(Body body);
  Response body(Bytes bytes);
  Response body(Vec<uint8_t> bytes);
  Response body(String text);
  Response body(const char* text);
  Response body();

  // 便捷静态
  static Response ok(String body);                    // 200 + body
  static Response ok(Bytes body);
  static Response ok();                               // 200, empty
  static Response created(String body);               // 201
  static Response no_content();                       // 204
  static Response bad_request(String body);           // 400
  static Response not_found();                        // 404
  static Response internal_server_error(String body); // 500

private:
  ResponseBuilder() = default;
  StatusCode  m_status   = StatusCode::Ok;
  HeaderMap   m_headers;
};
```

### 4.6 `Method`

```cpp
enum class Method {
  Get, Post, Put, Delete, Patch, Head, Options, Trace, Connect,
};

String          to_string(Method m);
Option<Method>  from_string(const String& s);
```

### 4.7 `StatusCode`

```cpp
enum class StatusCode : uint16_t {
  // 1xx
  Continue              = 100,
  SwitchingProtocols    = 101,
  // 2xx
  Ok                    = 200,
  Created               = 201,
  Accepted              = 202,
  NoContent             = 204,
  PartialContent        = 206,
  // 3xx
  MovedPermanently      = 301,
  Found                 = 302,
  SeeOther              = 303,
  NotModified           = 304,
  TemporaryRedirect     = 307,
  PermanentRedirect     = 308,
  // 4xx
  BadRequest            = 400,
  Unauthorized          = 401,
  Forbidden             = 403,
  NotFound              = 404,
  MethodNotAllowed      = 405,
  RequestTimeout        = 408,
  Conflict              = 409,
  Gone                  = 410,
  LengthRequired        = 411,
  PayloadTooLarge       = 413,
  UriTooLong            = 414,
  UnsupportedMediaType  = 415,
  RangeNotSatisfiable   = 416,
  TooManyRequests       = 429,
  // 5xx
  InternalServerError   = 500,
  NotImplemented        = 501,
  BadGateway            = 502,
  ServiceUnavailable    = 503,
  GatewayTimeout        = 504,
};

bool                is_informational(StatusCode s);
bool                is_success(StatusCode s);
bool                is_redirect(StatusCode s);
bool                is_client_error(StatusCode s);
bool                is_server_error(StatusCode s);

String              to_string(StatusCode s);
Option<StatusCode>  from_string(const String& s);
```

### 4.8 `HeaderMap`

```cpp
class HeaderMap {
public:
  HeaderMap() = default;
  HeaderMap(HeaderMap&&) noexcept            = default;
  HeaderMap& operator=(HeaderMap&&) noexcept = default;

  // 插入
  void insert(String key, String value);
  void insert(const char* key, const char* value);

  // 查询（大小写不敏感）
  Option<const String&> get(const String& key) const;
  Option<const String&> get(const char* key)   const;

  bool contains(const String& key) const;
  bool contains(const char* key)   const;

  // 多值（Set-Cookie 之类）
  class Values;
  Values get_all(const String& key) const;

  // 删除
  size_t erase(const String& key);

  // 状态
  bool   empty() const;
  size_t size()  const;

  // 迭代
  class const_iterator;
  const_iterator begin() const;
  const_iterator end()   const;

  static HeaderMap from_vec(Vec<std::pair<String, String>> entries);

private:
  // 平行数组，cache 友好；header 少时线性扫描最快
  Vec<String> m_keys;    // 存 lowercase
  Vec<String> m_values;
};
```

### 4.9 `RedirectPolicy`

```cpp
enum class RedirectPolicy {
  FollowUpTo10,   // 默认：最多 10 次
  FollowAll,      // 无限跟随
  None,           // 不跟随
};
```

### 4.10 `Error`

```cpp
class Error {
public:
  enum class Kind {
    Connect,
    Dns,
    Timeout,
    TooManyRedirects,
    InvalidUrl,
    Io,
    Protocol,
    Tls,
    Body,
  };

  Kind                 kind() const;
  const String&        message() const;
  Option<StatusCode>   status() const;     // HTTP 错误响应时的 status code

  bool is_connect() const;
  bool is_timeout() const;
  bool is_redirect() const;
  bool is_status_error() const;

  String to_string() const;
};
```

## 5. 文件结构

```
libxpp/xpp/
├── bytes.h              ← 新增：xpp::Bytes（引用计数字节块）
└── http/
    ├── method.h         ← enum class Method
    ├── status.h         ← enum class StatusCode : uint16_t
    ├── header.h         ← HeaderMap（Vec<(String, String)> 平行数组）
    ├── body.h           ← Body: Empty/Once/Channel
    ├── request.h        ← Request + RequestBuilder
    ├── response.h       ← Response + ResponseBuilder
    ├── client.h         ← Client + ClientBuilder
    ├── error.h          ← Error
    └── http.h           ← 顶层便捷函数 http::get/post/...
```

**10 个文件**。比旧分支的 13 个少（去掉 `bytes/` 模块、`test_server.h`、测试用的 `*_test.cpp` 移到 `tests/` 下）。

## 6. 跟旧分支的对比

| 维度 | 旧分支（`add-http-module`）| 新设计（本 proposal）|
|---|---|---|
| Body 模型 | push/pull 混合，反复 revert | 统一 pull，channel 桥接 |
| Body 抽象 | `std::function<ssize_t(char*, size_t)>`（同步）| `AsyncReader` 概念（`read → Promise<ssize_t>`）|
| `Response::text()` | 阻塞 while 循环 | `co_await body.bytes()` + UTF-8 校验 |
| `HeaderMap` | `std::multimap` | `Vec<(String, String)>` 平行数组 |
| `http::get(url)` | 没有，要写 4 行 builder | 有，一行 |
| Server handler | C 回调里 `.await()`（死锁风险）| 返回 `Promise`，handler 里自由 await |
| Bytes 类型 | `bytes::Bytes`（新模块）| `xpp::Bytes`（顶层，跟 String/Vec 并列）|
| 跟 io 模块 | 不兼容（用 `TryRead`）| 同构（`AsyncReader`）|
| 跟 hyper/reqwest API | 自创命名 | 对齐命名 |
| 文件数 | 13 | 10 |

## 7. 实现顺序

按"每步可验证"的原则：

```
Week 1-2  : bytes.h（Bytes 基础类型 + 单测）
Week 3-4  : http/method.h + http/status.h + http/header.h（基础类型 + 单测）
Week 5-6  : http/body.h（Body + ChannelReader 逻辑 + 单测）
Week 7-8  : http/request.h + http/response.h（Builder 模式 + 单测）
Week 9-10 : http/error.h + http/client.h（Adapter 桥接 libx + 集成测试）
Week 11-12: http/http.h（便捷函数）+ 文档 + 收尾
```

**先做 `bytes.h`**——其他所有文件都依赖它。`Bytes` 是基础类型，可以独立测试。

## 8. 未解决问题

### 8.1 libcurl 全局 init 开销

`http::get(url)` 等顶层便捷函数每次 new 一个 Client。如果 `xHttpClientCreate` 有 libcurl 的全局 init 开销（`curl_global_init`），要加 thread-local 默认 Client 复用。**待实测**。

### 8.2 Channel 容量

bounded 64 chunks × 4KB ≈ 256KB。太小会频繁背压，太大占内存。Rust hyper 用 8KB × 不限数量，Go 用 unbounded。**64 是初值，profile 后调整**。

### 8.3 Server 端设计

本 proposal 只覆盖 Client 侧。Server 端（`Server` + `Router` + `Handler`）以后再讨论，但会沿用同样的 push→pull 桥接模式。

### 8.4 H2 / TLS / WebSocket

libx 有 `proto_h2.c` / `tls.h` / `ws*.c`，但 xpp 还没 wrap。先做 HTTP/1 + plain TCP，H2/TLS/WS 作为后续 milestone。

### 8.5 JSON 支持

`Response::json<T>()` 依赖 serde 模块（还没做）。先返回 `Bytes` / `String`，JSON 之后加。

## 9. 使用示例

### 9.1 一次性 GET

```cpp
// reqwest 风格
auto resp = co_await xpp::http::get("https://example.com").unwrap();
auto text = co_await resp.text().unwrap();
```

### 9.2 自定义 header 的 POST

```cpp
auto client = xpp::http::Client::builder()
    .timeout(5s)
    .header("Authorization", "Bearer xxx")
    .build()
    .unwrap();

auto req = xpp::http::Request::builder()
    .method(xpp::http::Method::Post)
    .url("https://api.example.com/upload")
    .header("Content-Type", "application/json")
    .body(R"({"key":"value"})")
    .unwrap();

auto resp = co_await client.send(req).unwrap();
if (resp.status() != xpp::http::StatusCode::Ok) {
    // handle error
}
auto body = co_await resp.bytes().unwrap();
```

### 9.3 流式下载（边收边处理）

```cpp
auto resp = co_await xpp::http::get("https://large-file.example.com/big.bin").unwrap();
auto& body = resp.body();

char buf[4096];
while (true) {
    ssize_t n = co_await body.read(buf, sizeof(buf));
    if (n <= 0) break;
    process_chunk(buf, n);  // 边收边处理
}
```

### 9.4 跟 io 模块组合

```cpp
auto resp = co_await xpp::http::get("https://example.com").unwrap();

// 复用 io::read_all（Body 满足 AsyncReader 概念）
auto all_bytes = co_await xpp::io::read_all(resp.body());

// 复用 io::copy（流式转发到文件）
auto file = co_await xpp::fs::File::create("output.bin").unwrap();
co_await xpp::io::copy(resp.body(), file);
```

### 9.5 上传流式 body

```cpp
auto [tx, rx] = xpp::sync::mpsc::channel<xpp::Bytes>(64);

// 后台 fiber push 数据
xpp::spawn([tx]() -> xpp::Promise<void> {
    for (int i = 0; i < 100; i++) {
        char buf[1024];
        size_t n = generate_chunk(buf);
        co_await tx.send(xpp::Bytes::copy(buf, n));
    }
    tx.close();
});

auto req = xpp::http::Request::builder()
    .post("https://upload.example.com/stream")
    .body(xpp::http::Body::from_channel(rx))
    .unwrap();

auto resp = co_await client.send(req).unwrap();
```

## 10. 参考

- **hyper 0.12+**：`Body::channel()` 用 mpsc 桥接 push→pull
- **reqwest**：高层 API，`Response::bytes()` / `text()` / `json()`
- **axum**：Server handler 签名 `async fn(Request) -> Response`
- **bytes::Bytes**：引用计数不可变字节块，O(1) slice
- **hyper 0.10 → 0.12 重构**：跟我们的处境一样（push 回调 → pull async）

## 11. 决策记录

| 决策点 | 选择 | 理由 |
|---|---|---|
| `Shared` 单/多线程 | 编译期决定（`-DXPP_MT`）| 跟 xpp 其他类型一致 |
| `Body::bytes()` 返回类型 | `Result<Bytes>` | body 读取可能 I/O 错误 |
| `Bytes::to_string()` 返回类型 | `Result<String>` | 跟 `String::from_utf8` 一致 |
| `Bytes` 放哪 | `xpp::Bytes`（顶层）| 跟 String/Vec 并列，基础类型 |
| `Body::from_channel` 的 channel 类型 | `mpsc::Receiver<Bytes>` | Bytes 零拷贝切片 |
| `HeaderMap` 内部结构 | `Vec<(String, String)>` 平行数组 | header 少，线性扫描最快 |
| `Client::send` vs `request` | `send` | 更动词化，对齐 reqwest |
| `Response::body()` vs `into_body()` | 两者都有 | 对齐 hyper，borrow + move |
| 顶层 `http::get` | 有 | 对齐 reqwest，对齐 presentation 承诺 |
| URL 参数重载 | `String` / `const char*` / `std::string_view` | 兼容字面量、String、std 库 |
