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
 * Path parameters are injected by template: for a route pattern
 * `GET /users/:id`, a handler `[](Request req, String id)` gets `:id`
 * bound automatically (parameters in pattern order, type `String`).
 *
 * The request body arrives as a channel-backed `Body` (the C on_data
 * callback pushes chunks). A response Body of Once/Empty kind is sent
 * directly; channel (streaming) response bodies are not yet supported.
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
#include <xpp/fiber.h>
#include <xpp/http/client.h>
#include <xpp/http/request.h>
#include <xpp/http/response.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/result.h>
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

/// Per-route state: the (type-erased) handler invoker + path-parameter
/// names in pattern order. One per .route() registration.
struct RouteState {
  std::function<Promise<Result<Response>>(Request, Vec<String>)> invoke;
  Vec<String>                                                    param_names;
  String            pattern;          ///< raw "METHOD /path" pattern for libx mux
  class ServerImpl *server = nullptr; ///< back-pointer (stable heap address)
};

/// Per-request state: the request-body channel sender (fed by on_data).
/// Lives from on_request until the handler's response is written.
struct ReqState {
  Option<sync::mpsc::Sender<Bytes>> tx;
  ServerImpl                       *impl = nullptr; ///< owning server (to erase from m_reqs)
};

/* ── Signature traits (extract Ret/Args from a callable) ───────────── */

template <class T> struct function_traits;
template <class Ret, class... Args> struct function_traits<Ret(Args...)> {
  using Ret_t                   = Ret;
  static constexpr size_t arity = sizeof...(Args);
};
template <class Ret, class... Args>
struct function_traits<Ret (*)(Args...)> : function_traits<Ret(Args...)> {};
template <class Ret, class... Args>
struct function_traits<Ret (&)(Args...)> : function_traits<Ret(Args...)> {};
template <class Ret, class... Args>
struct function_traits<std::function<Ret(Args...)>> : function_traits<Ret(Args...)> {};
template <class Ret, class C, class... Args>
struct function_traits<Ret (C::*)(Args...) const> : function_traits<Ret(Args...)> {};
template <class Ret, class C, class... Args>
struct function_traits<Ret (C::*)(Args...)> : function_traits<Ret(Args...)> {};
template <class T> struct function_traits : function_traits<decltype(&T::operator())> {}; // lambdas

/* ── Handler return adaptation: Response / Result<Response> / Promise ── */

inline Promise<Result<Response>> adapt_handler_result(Response r) {
  return xpp::resolve(http::Result<Response>(xpp::ok, std::move(r)));
}
inline Promise<Result<Response>> adapt_handler_result(http::Result<Response> r) {
  return xpp::resolve(std::move(r));
}
inline Promise<Result<Response>> adapt_handler_result(Promise<http::Result<Response>> p) {
  return p;
}

/* ── Parameter injection (params[I] in pattern order, type String) ──── */

template <class H, size_t... I>
Promise<Result<Response>> invoke_with_params(H &h, Request req, const Vec<String> &params,
                                             std::index_sequence<I...>) {
  return adapt_handler_result(h(std::move(req), params[I]...));
}

/* ── Server internals (stable heap address — route back-pointers) ──── */

class ServerImpl {
public:
  xHttpServer                      m_server = nullptr;
  xHttpMux                         m_mux    = nullptr;
  String                           m_host;
  uint16_t                         m_port            = 0;
  uint64_t                         m_idle_timeout_ms = 60000;
  Vec<std::unique_ptr<RouteState>> m_routes; ///< owns RouteStates (mux arg)
  std::unordered_map<const xHttpCtx *, std::unique_ptr<ReqState>> m_reqs;
  Option<PromiseResolver<Result<void>>>                           m_stop_resolver;
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

inline void write_response(xHttpCtx *ctx, Result<Response> r) {
  if (r.is_err()) {
    xHttpCtxSetStatus(ctx, 500);
    xHttpCtxSend(ctx, "Internal Server Error", 21);
    return;
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
  if (body.is_channel()) {
    // Streaming (channel) response bodies are not yet supported.
    xHttpCtxSetStatus(ctx, 500);
    xHttpCtxSend(ctx, "Internal Server Error", 21);
    return;
  }
  Bytes bytes = body.into_once_bytes();
  xHttpCtxSend(ctx, reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

/* ── C callback trampolines ────────────────────────────────────────── */

inline int srv_on_request_cb(xHttpCtx *ctx, void *arg) {
  auto *route = static_cast<RouteState *>(arg);
  auto *impl  = route->server;
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

  // 3. Path parameters (pattern order).
  Vec<String> params;
  for (auto &name : route->param_names) {
    size_t      len = 0;
    auto        nb  = name.as_bytes();
    std::string nm(reinterpret_cast<const char *>(nb.data()), nb.size());
    const char *v = xHttpCtxParam(ctx, nm.c_str(), &len);
    if (v) {
      params.push(String::from_utf8(v, len).unwrap());
    } else {
      params.push(String());
    }
  }

  // 4. Per-request state: the channel sender, delivered to on_data /
  //    on_done as the arg via xHttpCtxSetUser (distinguishes concurrent
  //    requests on the same route). Owned by impl->m_reqs; erased in
  //    on_done (after the body channel is closed).
  auto req_state          = std::unique_ptr<ReqState>(new ReqState());
  req_state->tx           = xpp::some(std::move(tx));
  req_state->impl         = impl;
  const xHttpCtx *ctx_key = ctx;
  impl->m_reqs[ctx_key]   = std::move(req_state);
  xHttpCtxSetUser(ctx, impl->m_reqs[ctx_key].get());

  // 5. Run the handler on a fiber — xpp promises are poll-driven, so the
  //    handler's promise chain needs an awaiter. The fiber awaits the
  //    handler (suspending while the request body streams in via on_data
  //    / on_done), then writes the response. ctx stays valid until the
  //    response is sent (libx keeps the connection open when no response
  //    was written in on_done).
  // C++11 has no init-capture — move req/params into a heap holder that
  // the lambda can capture by copy.
  auto holder =
    std::make_shared<std::pair<Request, Vec<String>>>(std::move(req), std::move(params));
  auto run = [impl, ctx_key, route, holder]() {
    Result<Response> r = route->invoke(std::move(holder->first), std::move(holder->second)).await();
    write_response(const_cast<xHttpCtx *>(ctx_key), std::move(r));
  };
  xpp::fiber(0, std::move(run));
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
      if (m_impl->m_server) xHttpServerDestroy(m_impl->m_server);
      if (m_impl->m_mux) xHttpMuxDestroy(m_impl->m_mux);
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
   * @brief Register a route.
   *
   * @p pattern is "METHOD /path" or "/path" (any method); ":name"
   * segments become handler parameters, injected in pattern order.
   * Handler signature: `Ret(Request, String, ...)` with Ret one of
   * Response / Result<Response> / Promise<Result<Response>>.
   */
  template <class H> ServerBuilder &route(const char *pattern, H &&handler) {
    // Parse pattern → param names (in order) and store the handler.
    auto names   = parse_pattern_params(pattern);
    using H_t    = typename std::decay<H>::type;
    using Traits = _::function_traits<H_t>;
    using Ret    = typename Traits::Ret_t;
    static_assert(Traits::arity >= 1, "handler must take at least (Request)");
    XPP_ASSERT(Traits::arity == 1 + names.len(),
               "handler parameter count must match :param count in the pattern");

    H_t  stored  = std::forward<H>(handler);
    auto invoker = [stored](Request req, Vec<String> params) -> Promise<Result<Response>> {
      return _::invoke_with_params(stored, std::move(req), params,
                                   std::make_index_sequence<Traits::arity - 1>());
    };
    auto state         = std::unique_ptr<_::RouteState>(new _::RouteState());
    state->invoke      = std::move(invoker);
    state->param_names = std::move(names);
    state->pattern     = String::from_utf8(pattern).unwrap();
    m_routes.push(std::move(state));
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

    impl->m_mux = xHttpMuxCreate();
    if (!impl->m_mux) {
      return Result<Server>(
        xpp::err, Error(Error::Kind::Io, String::from_utf8("xHttpMuxCreate failed").unwrap()));
    }
    for (auto &route : m_routes) {
      route->server       = impl.get();
      xHttpRouteConf conf = {};
      auto           pb   = route->pattern.as_bytes();
      std::string    pat(reinterpret_cast<const char *>(pb.data()), pb.size());
      conf.pattern    = pat.c_str();
      conf.on_request = _::srv_on_request_cb;
      conf.on_data    = _::srv_on_data_cb;
      conf.on_done    = _::srv_on_done_cb;
      conf.arg        = route.get();
      // Register with libx mux using the raw pattern (param names live in
      // RouteState for injection).
      xErrno rc = xHttpMuxHandle(impl->m_mux, &conf);
      if (rc != xErrno_Ok) {
        return Result<Server>(
          xpp::err, Error(Error::Kind::Io, String::from_utf8("xHttpMuxHandle failed").unwrap()));
      }
    }

    xHttpServerConf sconf = {};
    sconf.resolve         = xHttpMuxResolve;
    sconf.router          = impl->m_mux;
    sconf.idle_timeout_ms = static_cast<int>(impl->m_idle_timeout_ms);
    impl->m_server        = xHttpServerCreate(&sconf);
    if (!impl->m_server) {
      xHttpMuxDestroy(impl->m_mux);
      impl->m_mux = nullptr;
      return Result<Server>(
        xpp::err, Error(Error::Kind::Io, String::from_utf8("xHttpServerCreate failed").unwrap()));
    }

    // Move route ownership into the impl (mux arg pointers stay valid).
    impl->m_routes = std::move(m_routes);
    return Result<Server>(xpp::ok, Server(std::move(impl)));
  }

private:
  static Vec<String> parse_pattern_params(const char *pattern) {
    Vec<String> names;
    const char *p = pattern;
    while (*p) {
      if (*p == ':') {
        const char *start = ++p;
        while (*p && *p != '/' && *p != ' ')
          ++p;
        names.push(String::from_utf8(start, static_cast<size_t>(p - start)).unwrap());
      } else {
        ++p;
      }
    }
    return names;
  }

  String                              m_host            = String::from_utf8("127.0.0.1").unwrap();
  uint16_t                            m_port            = 8080;
  uint64_t                            m_idle_timeout_ms = 60000;
  Vec<std::unique_ptr<_::RouteState>> m_routes;
};

inline ServerBuilder Server::builder() {
  return ServerBuilder();
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_SERVER_H
