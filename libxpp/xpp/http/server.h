/*
 * Copyright 2025-2026 The libxpp Authors. All rights reserved.
 *
 * server.h — HTTP server (xpp-style RAII wrapper around libx xHttpServer)
 *
 * Uses the pull model: handlers return Reply headers + optional body;
 * the server pulls body data via the xHttpServer read callback.
 */

#ifndef XPP_HTTP_SERVER_H
#define XPP_HTTP_SERVER_H

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <xpp/handle.h>
#include <xpp/http/response.h>
#include <xpp/io/traits.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/result.h>

#include <x/base/error.h>
#include <x/http/client.h> /* xHttpCtx, xHttpInitFunc, ... */
#include <x/http/server.h> /* xHttpServer, xHttpServerConf, xHttpRouteConf, xHttpServerReadFunc */

namespace xpp {
namespace http {

// ═════════════════════════════════════════════════════════════════════
// Writer — thin wrapper for streaming response body (server side).
//
// TODO: re-integrate with the pull model (currently streaming bodies
// are buffered before being sent).
// ═════════════════════════════════════════════════════════════════════

class Writer {
public:
  explicit Writer(xHttpCtx *ctx) : m_ctx(ctx) {}

  void write(const char *data, size_t len) {
    // TODO: integrate with pull model — buffer data for on_read to consume.
    m_buf.append(data, len);
  }
  void write(const std::string &s) {
    write(s.c_str(), s.size());
  }

  /// Move accumulated data out for the server to send.
  std::string take_buffer() {
    return std::move(m_buf);
  }

private:
  xHttpCtx   *m_ctx;
  std::string m_buf;
};

// ═════════════════════════════════════════════════════════════════════
// IncomingRequest — server-side request (method, url, headers, params)
// ═════════════════════════════════════════════════════════════════════

class IncomingRequest {
public:
  const std::string &method() const {
    return m_method;
  }
  const std::string &path() const {
    return m_path;
  }

  /// Look up a header by name (case-insensitive).
  Option<std::string> header(const std::string &name) const {
    return m_headers.get(name);
  }

  /// Look up a route parameter (e.g. "id" for "/users/:id").
  std::string param(const std::string &name) const {
    size_t      len   = 0;
    const char *value = xHttpCtxParam(m_ctx, name.c_str(), &len);
    if (value && len > 0) return std::string(value, len);
    return {};
  }

  /// Raw header string (for logging, etc.)
  const char *headers_raw() const {
    return m_ctx->headers;
  }

  friend class Router;
  xHttpCtx                               *m_ctx;
  std::string                             m_method;
  std::string                             m_path;
  HeaderMap m_headers;

  IncomingRequest(xHttpCtx *ctx, const char *method, const char *path)
      : m_ctx(ctx), m_method(method), m_path(path) {}

  /// Parse NUL-terminated request headers into m_headers.
  void parse_headers(const char *raw, size_t len) {
    if (!raw || !len) return;
    const char *end = raw + len;
    while (raw < end) {
      const char *line = raw;
      while (raw < end && *raw != '\n')
        ++raw;
      if (raw > line) {
        std::string s(line, static_cast<size_t>(raw - line));
        auto        colon = s.find(':');
        if (colon != std::string::npos) {
          std::string key = s.substr(0, colon);
          std::string val = s.substr(colon + 1);
          // trim leading space from value
          if (!val.empty() && val[0] == ' ') val.erase(0, 1);
          m_headers.insert(std::move(key), std::move(val));
        }
      }
      if (raw < end) ++raw; // skip '\n'
    }
  }
};

// ═════════════════════════════════════════════════════════════════════
// Router
// ═════════════════════════════════════════════════════════════════════

using Handler = std::function<Promise<Result<Response>>(IncomingRequest &)>;

namespace _ {

/// Per-route state stored on the heap. Passed as `arg` to the C
/// callbacks (on_request, on_read).
struct RouteState {
  Handler handler;
  Option<std::function<size_t(char *, size_t)>> body_reader;

  explicit RouteState(Handler h) : handler(std::move(h)) {}
};

} // namespace _

class Router {
public:
  Router() : m_mux(xHttpMuxCreate()) {}
  ~Router() {
    for (auto *rs : m_routes)
      delete rs;
  }

  Router(Router &&) noexcept            = default;
  Router &operator=(Router &&) noexcept = default;

  Router(const Router &)            = delete;
  Router &operator=(const Router &) = delete;

  /// Access the raw xHttpMux handle (for Server integration).
  xHttpMux raw() const {
    return m_mux.get();
  }

  /// Register a route with a handler.
  ///
  /// The handler returns Promise<Result<Response>>.  sync handlers use
  /// Response::ok("x").into_promise().  The Router extracts the
  /// status/headers in on_request, and provides body data via on_read.
  ///
  /// Pattern examples: "/hello", "GET /hello", "POST /echo", "/users/:id".
  Router &route(const std::string &pattern, Handler handler) {
    auto *rs = new _::RouteState(std::move(handler));
    m_routes.push_back(rs);

    xHttpRouteConf conf = {};
    conf.pattern        = pattern.c_str();
    conf.on_request     = on_request;
    conf.on_read        = on_read;
    conf.arg            = rs;
    xHttpMuxHandle(m_mux.get(), &conf);
    return *this;
  }

private:
  friend class Server;

  struct Deleter {
    void deallocate(void *p, Layout) const noexcept {
      if (p) xHttpMuxDestroy(static_cast<xHttpMux>(p));
    }
  };
  OwnedHandle<Deleter>         m_mux;
  std::vector<_::RouteState *> m_routes; // owned, one per route()

  // ── xHttpInitFunc: called after request headers are parsed ───────
  static int on_request(xHttpCtx *ctx, void *arg) {
    auto           *rs = static_cast<_::RouteState *>(arg);
    IncomingRequest req(ctx, ctx->method, ctx->url);
    if (ctx->headers && ctx->headers_len) {
      req.parse_headers(ctx->headers, ctx->headers_len);
    }

    auto result = rs->handler(req).await();
    if (result.is_err()) return -1;

    Response &reply = result.unwrap();

    // Write status + headers to xHttpCtx.
    xHttpCtxSetStatus(ctx, reply.status());
    for (auto &[key, value] : reply.headers()) {
      xHttpCtxSetHeader(ctx, key.c_str(), value.c_str());
    }

    // Store body for on_read to pull.
    if (reply.has_body()) {
      auto inner = std::move(reply.take_try_read()).unwrap_unchecked();
      rs->body_reader = Option<std::function<size_t(char *, size_t)>>(
        [fn = std::move(inner)](char *buf, size_t size) -> size_t {
          ssize_t n = fn(buf, size);
          return n > 0 ? static_cast<size_t>(n) : 0;
        });
    } else {
      xHttpCtxSetHeader(ctx, "Content-Length", "0");
    }

    return 0;
  }

  // ── xHttpReadFunc: pull response body data ───────────────────────
  static size_t on_read(char *buf, size_t bufsize, void *arg) {
    auto *rs = static_cast<_::RouteState *>(arg);

    if (rs->body_reader.is_some()) {
      return rs->body_reader.unwrap_unchecked()(buf, bufsize);
    }

    return 0; // no body → EOF
  }
};

// ═════════════════════════════════════════════════════════════════════
// Server — thin RAII wrapper around xHttpServer.
// ═════════════════════════════════════════════════════════════════════

class Server {
public:
  static Result<Server> bind(const char *addr);

  Server() = default;
  Server(Server &&other) noexcept
    : m_server(std::move(other.m_server)),
      m_host(std::move(other.m_host)),
      m_port(other.m_port),
      m_shutdown_resolver(std::move(other.m_shutdown_resolver)) {
    XPP_ASSERT(!m_shutdown_resolver.is_some(),
               "cannot move a Server while serving — call serve() after the move");
  }
  Server &operator=(Server &&other) noexcept {
    if (this != &other) {
      m_server            = std::move(other.m_server);
      m_host              = std::move(other.m_host);
      m_port              = other.m_port;
      m_shutdown_resolver = std::move(other.m_shutdown_resolver);
      XPP_ASSERT(!m_shutdown_resolver.is_some(),
                 "cannot move a Server while serving");
    }
    return *this;
  }

  /** Start listening and return a Promise that resolves when the server
   *  is destroyed (RAII — on_shutdown callback). */
  Promise<Result<void>> serve(Router &router);

private:
  struct Deleter {
    void deallocate(void *p, Layout) const noexcept {
      if (p) xHttpServerDestroy(static_cast<xHttpServer>(p));
    }
  };
  Server(const Server &)            = delete;
  Server &operator=(const Server &) = delete;

  // m_shutdown_resolver MUST be declared before m_server so it outlives
  // the OwnedHandle — when m_server destructor fires on_shutdown, the
  // resolver is still alive.
  Option<PromiseResolver<Result<void>>>  m_shutdown_resolver;
  OwnedHandle<Deleter>                   m_server;
  std::string                            m_host;
  uint16_t                               m_port = 0;
};

/* ── Server::bind ─────────────────────────────────────────────────── */

inline Result<Server> Server::bind(const char *addr) {
  const char *colon = std::strrchr(addr, ':');
  if (!colon) return Result<Server>(xpp::err, Error::builder("invalid address"));

  Server srv;
  srv.m_host.assign(addr, static_cast<size_t>(colon - addr));
  if (srv.m_host.empty()) srv.m_host = "0.0.0.0";
  srv.m_port = static_cast<uint16_t>(std::strtoul(colon + 1, nullptr, 10));

  return Result<Server>(xpp::ok, std::move(srv));
}

/* ── Server::serve ────────────────────────────────────────────────── */

Promise<Result<void>> Server::serve(Router &router) {
  auto [p, r] = async<Result<void>>();
  m_shutdown_resolver = Option<PromiseResolver<Result<void>>>(std::move(r));

  xHttpServerConf conf = {};
  conf.resolve         = xHttpMuxResolve;
  conf.router          = router.raw();
  conf.idle_timeout_ms = 60000;
  conf.on_shutdown     = [](void *arg) {
    static_cast<Server *>(arg)->m_shutdown_resolver.unwrap_unchecked().resolve(
      xpp::Result<void, Error>(xpp::ok));
  };
  conf.shutdown_arg    = this;

  OwnedHandle<Deleter> h(xHttpServerCreate(&conf));
  if (!h.get()) {
    m_shutdown_resolver.unwrap_unchecked().resolve(
      Result<void>(xpp::err, Error::builder("failed to create server")));
    m_shutdown_resolver = none;
    return std::move(p);
  }

  xErrno err = xHttpServerListen(h.get(), m_host.c_str(), m_port);
  if (err != xErrno_Ok) {
    m_shutdown_resolver.unwrap_unchecked().resolve(
      Result<void>(xpp::err, Error::builder("listen failed")));
    m_shutdown_resolver = none;
    return std::move(p); // h destroyed, on_shutdown fires but resolver is moved out
  }

  m_server = std::move(h);
  return std::move(p);
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_SERVER_H
