/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server.h - xpp::http::Server + ServerBuilder.
 *
 * Asynchronous HTTP/1.1 + HTTP/2 server wrapping libx's xHttpServer
 * (llhttp/nghttp2 + xEventLoop). Handlers are async-first: a route
 * handler receives an `xpp::http::Request` (reusing the client Request
 * type — hyper style) and returns `Response`, `Result<Response>`, or
 * `Promise<Result<Response>>`; the last lets the handler `co_await`
 * the request body or external I/O before responding.
 *
 * Routing, path parameters, 404/405, and middleware all live in the
 * composable `Router` (router.h — tower/axum-aligned). The server mounts
 * one Router as its sole route via a custom C resolver; `.route()` on
 * the builder registers into that Router, `.layer()` adds middleware,
 * and `.router(r)` hands over a pre-composed one.
 *
 * Lifecycle: `Server::builder().route(...).bind(host, port).build()` →
 * `server.serve()` returns a `Promise<Result<void>>` that resolves when
 * `server.stop()` is called (`co_await server.serve()` = run until
 * stopped). `server.port()` reports the actual port (bind(0) for tests).
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_HTTP_SERVER_H
#define XPP_HTTP_SERVER_H

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>

#include <xpp/arc.h>
#include <xpp/http/client.h>
#include <xpp/http/request.h>
#include <xpp/http/response.h>
#include <xpp/http/router.h>
#include <xpp/option.h>
#include <xpp/own.h>
#include <xpp/promise.h>
#include <xpp/result.h>
#include <xpp/spawn.h>
#include <xpp/string.h>
#include <xpp/sync/mpsc.h>
#include <xpp/vec.h>

#include <x/base/error.h>
#include <x/http/server.h>

namespace xpp {
namespace http {

class ServerBuilder;
class Server;

namespace _ {

class ServerImpl;

/// Shared server lifetime flag. The Server sets destroyed=true before
/// tearing down; in-flight handler chains hold an Arc and check it
/// instead of touching the freed ctx.
class ServerLifetime {
public:
  bool destroyed = false;
};

/// Per-request state: the request-body channel sender (fed by on_data).
/// Lives from on_request until the handler's response is written.
struct ReqState {
  Option<sync::mpsc::Sender<Bytes>> tx;
  ServerImpl                       *impl = nullptr; ///< owning server (to erase from m_reqs)
};

/**
 * @brief The server's single C-level route: the Router.
 *
 * The C resolver always returns this entry's info — matching, params,
 * 404/405 and middleware all happen C++-side in Router::operator().
 */
struct RouterDispatch {
  Router        *router = nullptr;
  ServerImpl    *impl   = nullptr;
  xHttpRouteInfo info   = {};
};

inline const xHttpRouteInfo *xpp_router_resolve(void *router, xHttpCtx * /*ctx*/) {
  return &static_cast<RouterDispatch *>(router)->info;
}

/* ── Server internals (stable heap address — dispatch back-pointers) ─ */

class ServerImpl {
public:
  xHttpServer                                         m_server = nullptr;
  String                                              m_host;
  uint16_t                                            m_port            = 0;
  uint64_t                                            m_idle_timeout_ms = 60000;
  Router                                              m_router; ///< all routing + middleware
  RouterDispatch                                      m_dispatch;
  std::unordered_map<const xHttpCtx *, Own<ReqState>> m_reqs;
  Option<PromiseResolver<Result<void>>>               m_stop_resolver;
  Arc<ServerLifetime>                                 lifetime = Arc<ServerLifetime>::make();
};

/* ── Raw header parsing ("Name: Value\r\n", no request line) ───────── */

inline HeaderMap parse_server_headers(const char *raw, size_t len) {
  HeaderMap map;
  if (!raw || len == 0) return map;
  const char *p   = raw;
  const char *end = raw + len;
  while (p < end) {
    const char *line_end = p;
    while (line_end < end && line_end[0] != '\r' && line_end[0] != '\n')
      ++line_end;
    if (line_end == p) break; // blank line
    const char *colon = p;
    while (colon < line_end && *colon != ':')
      ++colon;
    if (colon < line_end) {
      const char *val = colon + 1;
      while (val < line_end && (*val == ' ' || *val == '\t'))
        ++val;
      map.insert(String::from_utf8(p, static_cast<size_t>(colon - p)).unwrap(),
                 String::from_utf8(val, static_cast<size_t>(line_end - val)).unwrap());
    }
    if (line_end >= end) break;
    p = line_end + ((line_end[0] == '\r' && line_end + 1 < end && line_end[1] == '\n') ? 2 : 1);
  }
  return map;
}

/* ── Response writing (handler completed) ──────────────────────────── */

namespace {
constexpr size_t kStreamChunk = 4096;
}

/**
 * @brief Stream a channel body via xHttpCtxWrite/xHttpCtxEndStream.
 *
 * Recursively reads chunks from the channel (waker-driven — the promise
 * chain suspends when the channel is empty) and writes each to the wire.
 * Each hop allocates a fresh heap buffer so the read() buffer outlives
 * the pending recv. Stops (drops the rest) if the server was destroyed
 * or the connection write fails.
 */
inline Promise<void> stream_channel_body(xHttpCtx *ctx, Arc<ServerLifetime> lifetime,
                                         Arc<Body> body) {
  auto buf = Arc<Vec<char>>::make();
  buf->resize(kStreamChunk, '\0');
  return body->read(buf->data(), kStreamChunk)
    .then([ctx, lifetime, body, buf](ssize_t n) -> Promise<void> {
      if (lifetime->destroyed) return xpp::resolve(); // server torn down
      if (n <= 0) {
        xHttpCtxEndStream(ctx); // EOF — finalize (Connection: close)
        return xpp::resolve();
      }
      xErrno rc = xHttpCtxWrite(ctx, buf->data(), static_cast<size_t>(n));
      if (rc != xErrno_Ok) return xpp::resolve(); // connection gone — drop
      return stream_channel_body(ctx, lifetime, body);
    });
}

inline Promise<void> write_response(xHttpCtx *ctx, Arc<ServerLifetime> lifetime,
                                    Result<Response> r) {
  if (r.is_err()) {
    xHttpCtxSetStatus(ctx, 500);
    xHttpCtxSend(ctx, "Internal Server Error", 21);
    return xpp::resolve();
  }
  Response resp = std::move(r).unwrap();
  xHttpCtxSetStatus(ctx, resp.status_code());
  for (auto it = resp.headers().begin(); it != resp.headers().end(); ++it) {
    auto        kv = *it;
    auto        kb = kv.first.as_bytes();
    auto        vb = kv.second.as_bytes();
    std::string n(reinterpret_cast<const char *>(kb.data()), kb.size());
    std::string v(reinterpret_cast<const char *>(vb.data()), vb.size());
    xHttpCtxSetHeader(ctx, n.c_str(), v.c_str());
  }
  Body body = std::move(resp).into_body();
  if (!body.is_channel()) {
    Bytes bytes = body.into_once_bytes();
    xHttpCtxSend(ctx, reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return xpp::resolve();
  }
  // Channel body — stream it. Arc<Body> keeps the reader alive across the
  // recursive read/write chain.
  Arc<Body> shared = Arc<Body>::make(std::move(body));
  return stream_channel_body(ctx, lifetime, shared);
}

/* ── C callback trampolines ────────────────────────────────────────── */

inline int srv_on_request_cb(xHttpCtx *ctx, void *arg) {
  auto *d    = static_cast<RouterDispatch *>(arg);
  auto *impl = d->impl;
  if (!impl || !impl->m_server) return 1;

  // 1. Request-body channel (bounded 256; handler consumes via Body).
  auto channel_pair = sync::mpsc::channel<Bytes>(256);
  auto tx           = std::move(channel_pair.first);
  auto rx           = std::move(channel_pair.second);

  // 2. Build the Request (method / request-target / headers / channel body).
  Method::Value method = Method::Get;
  if (ctx->method) {
    auto m = Method::from_string(String::from_utf8(ctx->method).unwrap());
    if (m.is_some()) method = m.unwrap();
  }
  RequestBuilder builder = std::move(
    Request::builder().method(method).url(String::from_utf8(ctx->url ? ctx->url : "/").unwrap()));
  HeaderMap headers = parse_server_headers(ctx->headers, ctx->headers_len);
  for (auto it = headers.begin(); it != headers.end(); ++it) {
    auto kv = *it;
    builder.header(kv.first, kv.second);
  }
  Request req = builder.body(Body::from_channel(std::move(rx))).unwrap();

  // Path parameters are extracted by the Router (it also stores them on
  // the Request for req.param() and runs the middleware chain).

  // 3. Per-request state: the channel sender, delivered to on_data /
  //    on_done as the arg via xHttpCtxSetUser (distinguishes concurrent
  //    requests on the same route). Owned by impl->m_reqs; erased in
  //    on_done (after the body channel is closed).
  auto req_state          = Own<ReqState>(new ReqState());
  req_state->tx           = xpp::some(std::move(tx));
  req_state->impl         = impl;
  const xHttpCtx *ctx_key = ctx;
  impl->m_reqs[ctx_key]   = std::move(req_state);
  xHttpCtxSetUser(ctx, impl->m_reqs[ctx_key].get());

  // 4. Dispatch through the Router via xpp::spawn — matching, params,
  //    middleware, and the handler run as a waker-driven promise chain
  //    on the event loop (no per-request fiber stack). The router's
  //    dispatch composes synchronously here (invoke copies captured by
  //    value), so the chain stays self-contained even if the Server is
  //    destroyed while the handler is in flight.
  auto router = d->router;
  // C++11 has no move-capture — hold the Request on the heap.
  auto holder   = Arc<Request>::make(std::move(req));
  auto lifetime = impl->lifetime; // Arc — outlives the server
  xpp::spawn([router, lifetime, ctx_key, holder]() -> Promise<void> {
    return (*router)(std::move(*holder))
      .then([lifetime, ctx_key](Result<Response> r) -> Promise<void> {
        // Server destroyed while the handler was running (ctx freed) —
        // drop the response instead of touching it.
        if (lifetime->destroyed) return xpp::resolve();
        return write_response(const_cast<xHttpCtx *>(ctx_key), lifetime, std::move(r));
      });
  });
  return 0;
}

inline int srv_on_data_cb(const char *data, size_t len, void *arg) {
  auto *req_state = static_cast<ReqState *>(arg);
  if (!req_state || req_state->tx.is_none()) return 0; // discard
  Bytes chunk = Bytes::copy(data, len);
  auto  r     = req_state->tx.unwrap().try_send(std::move(chunk));
  return r.is_err() ? 1 : 0; // channel full → 413 (request body overflow)
}

inline void srv_on_done_cb(xHttpCtx *ctx, void *arg) {
  auto *req_state = static_cast<ReqState *>(arg);
  if (req_state && req_state->tx.is_some()) {
    req_state->tx.unwrap().close(); // body EOF for the handler's Body
  }
  // Release per-request state. The handler may still be running (it sees
  // channel EOF) — it no longer needs req_state, only ctx (valid until
  // the response is written).
  if (req_state && req_state->impl) {
    req_state->impl->m_reqs.erase(ctx);
  }
}

} // namespace _

/**
 * @brief Asynchronous HTTP server.
 *
 * @code
 *   auto server = Server::builder()
 *                   .route("GET /users/:id",
 *                          [](Request req, String id) { return Response::ok(id); })
 *                   .route("/echo", [](Request req) -> Promise<Result<Response>> {
 *                     auto body = co_await req.into_body().bytes();
 *                     return Response::ok(body.unwrap());
 *                   })
 *                   .bind("127.0.0.1", 8080)
 *                   .build()
 *                   .unwrap();
 *   co_await server.serve();   // run until stop()
 * @endcode
 */
class Server {
public:
  Server() = default;
  ~Server() {
    if (m_impl) {
      m_impl->lifetime->destroyed = true; // in-flight spawn chains must not touch ctx
      if (m_impl->m_server) xHttpServerDestroy(m_impl->m_server);
    }
  }
  Server(Server &&) noexcept            = default;
  Server &operator=(Server &&) noexcept = default;
  Server(const Server &)                = delete;
  Server &operator=(const Server &)     = delete;

  static ServerBuilder builder();

  /**
   * @brief Start listening (synchronously) and return a Promise that
   * resolves when `stop()` is called.
   *
   * `co_await server.serve()` runs the server until stopped. The listen
   * happens immediately (port() is usable right away); the returned
   * Promise resolves `Err` if listen failed (e.g. port in use).
   */
  Promise<Result<void>> serve() {
    if (!m_impl) return xpp::resolve(Result<void>(xpp::err, err_listen()));
    auto        hb = m_impl->m_host.as_bytes();
    std::string hst(reinterpret_cast<const char *>(hb.data()), hb.size());
    xErrno      rc = xHttpServerListen(m_impl->m_server, hst.c_str(), m_impl->m_port);
    if (rc != xErrno_Ok) return xpp::resolve(Result<void>(xpp::err, err_listen()));
    m_impl->m_port          = xHttpServerPort(m_impl->m_server);
    auto pr                 = xpp::async<Result<void>>();
    m_impl->m_stop_resolver = xpp::some(std::move(pr.second));
    return std::move(pr.first);
  }

  /** @brief Stop the server: resolves the Promise returned by serve(). */
  void stop() {
    if (m_impl && m_impl->m_stop_resolver.is_some()) {
      m_impl->m_stop_resolver.unwrap().resolve(Result<void>(xpp::ok));
      m_impl->m_stop_resolver = none;
    }
  }

  /** @brief The actual bound port (after serve(); bind(0) supported). */
  uint16_t port() const noexcept {
    return m_impl ? m_impl->m_port : 0;
  }

private:
  static Error err_listen() {
    return Error(Error::Kind::Io, String::from_utf8("xHttpServerListen failed").unwrap());
  }

  friend class ServerBuilder;
  explicit Server(Arc<_::ServerImpl> impl) : m_impl(std::move(impl)) {}

  Arc<_::ServerImpl> m_impl;
};

/**
 * @brief Fluent builder for `Server`.
 */
class ServerBuilder {
public:
  ServerBuilder()                                     = default;
  ServerBuilder(ServerBuilder &&) noexcept            = default;
  ServerBuilder &operator=(ServerBuilder &&) noexcept = default;
  ServerBuilder(const ServerBuilder &)                = delete;
  ServerBuilder &operator=(const ServerBuilder &)     = delete;

  /**
   * @brief Register a route (into the builder's Router).
   *
   * @p pattern is "METHOD /path" or "/path" (any method); ":name"
   * segments become handler parameters, injected in pattern order.
   * Handler signature: `Ret(Request, String, ...)` with Ret one of
   * Response / Result<Response> / Promise<Result<Response>>.
   */
  template <class H> ServerBuilder &route(const char *pattern, H &&handler) {
    m_router.route(pattern, std::forward<H>(handler));
    return *this;
  }

  /**
   * @brief Hand over a pre-composed Router (replaces routes registered
   *        on this builder so far).
   */
  ServerBuilder &router(Router r) {
    m_router = std::move(r);
    return *this;
  }

  /**
   * @brief Add a middleware layer (into the builder's Router).
   *
   * Registration order follows tower's ServiceBuilder: the first layer
   * registered is the outermost. See `Router::layer`.
   */
  template <class M> ServerBuilder &layer(M &&m) {
    m_router.layer(std::forward<M>(m));
    return *this;
  }

  /** @brief Listen address (default 127.0.0.1:8080). Port 0 = kernel-assigned. */
  ServerBuilder &bind(const char *host, uint16_t port) {
    m_host = String::from_utf8(host).unwrap();
    m_port = port;
    return *this;
  }

  /** @brief Connection idle timeout in ms (0 = no timeout). Default 60000. */
  ServerBuilder &idle_timeout(uint64_t ms) {
    m_idle_timeout_ms = ms;
    return *this;
  }

  /**
   * @brief Create the server (binds to the current EventLoop). Does not
   * listen — call Server::serve().
   */
  Result<Server> build() {
    auto impl               = Arc<_::ServerImpl>::make();
    impl->m_host            = m_host;
    impl->m_port            = m_port;
    impl->m_idle_timeout_ms = m_idle_timeout_ms;

    // The Router is the server's single route: the C resolver always
    // returns its dispatch entry; matching, params, 404/405, and the
    // middleware chain all run C++-side in Router::operator().
    impl->m_router                   = std::move(m_router);
    impl->m_dispatch.router          = &impl->m_router;
    impl->m_dispatch.impl            = impl.get();
    impl->m_dispatch.info.on_request = _::srv_on_request_cb;
    impl->m_dispatch.info.on_data    = _::srv_on_data_cb;
    impl->m_dispatch.info.on_done    = _::srv_on_done_cb;
    impl->m_dispatch.info.arg        = &impl->m_dispatch;

    xHttpServerConf sconf = {};
    sconf.resolve         = _::xpp_router_resolve;
    sconf.router          = &impl->m_dispatch;
    sconf.idle_timeout_ms = static_cast<int>(impl->m_idle_timeout_ms);
    impl->m_server        = xHttpServerCreate(&sconf);
    if (!impl->m_server) {
      return Result<Server>(
        xpp::err, Error(Error::Kind::Io, String::from_utf8("xHttpServerCreate failed").unwrap()));
    }

    return Result<Server>(xpp::ok, Server(std::move(impl)));
  }

private:
  String   m_host            = String::from_utf8("127.0.0.1").unwrap();
  uint16_t m_port            = 8080;
  uint64_t m_idle_timeout_ms = 60000;
  Router   m_router;
};

inline ServerBuilder Server::builder() {
  return ServerBuilder();
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_SERVER_H
