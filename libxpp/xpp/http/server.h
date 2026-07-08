/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server.h - xpp::http server: Router, Request, ResponseBuilder, Server.
 *
 * Wraps libx xHttpServer / xHttpMux / xHttpCtx. Handler bridge via
 * ServerAdapter (heap-stored per route, one per route registration).
 *
 * Entry point (Server TBD):
 *
 *   Router router;
 *   router.route("/hello", [](Request &req) {
 *     req.respond().status(200).body("Hello, world!");
 *   });
 *
 * Aligned with axum request/response patterns.
 */

#ifndef XPP_HTTP_SERVER_H
#define XPP_HTTP_SERVER_H

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <xpp/bytes/bytes.h>
#include <xpp/event.h>
#include <xpp/handle.h>
#include <xpp/http/error.h>
#include <xpp/option.h>
#include <xpp/promise.h>

#include <x/http/server.h>

namespace xpp {
namespace http {

// ═════════════════════════════════════════════════════════════════════
// ResponseBuilder
// ═════════════════════════════════════════════════════════════════════

class ResponseBuilder {
public:
  ResponseBuilder &status(int code) {
    xHttpCtxSetStatus(m_ctx, code);
    return *this;
  }

  ResponseBuilder &header(const std::string &key, const std::string &value) {
    xHttpCtxSetHeader(m_ctx, key.c_str(), value.c_str());
    return *this;
  }

  /// Send a complete buffered response. Must be called at most once.
  void body(const std::string &s) {
    xHttpCtxSend(m_ctx, s.data(), s.size());
  }

  void body(bytes::Bytes b) {
    xHttpCtxSend(m_ctx, reinterpret_cast<const char *>(b.data()), b.size());
  }

  /// Begin a streaming response. The callback receives a Writer that
  /// can call write() repeatedly. End of stream is signaled by destroying
  /// the Writer (implicitly calls xHttpCtxEndStream via RAII).
  void stream(std::function<void(class Writer &)> fn);

private:
  friend class Request;
  explicit ResponseBuilder(xHttpCtx *ctx) : m_ctx(ctx) {}
  xHttpCtx *m_ctx;
};

// ═════════════════════════════════════════════════════════════════════
// Writer — streaming response body
// ═════════════════════════════════════════════════════════════════════

class Writer {
public:
  void write(const std::string &s) {
    xHttpCtxWrite(m_ctx, s.data(), s.size());
  }

  void write(bytes::Bytes b) {
    xHttpCtxWrite(m_ctx, reinterpret_cast<const char *>(b.data()), b.size());
  }

  ~Writer() {
    xHttpCtxEndStream(m_ctx);
  }

  Writer(Writer &&)                 = delete;
  Writer &operator=(Writer &&)      = delete;
  Writer(const Writer &)            = delete;
  Writer &operator=(const Writer &) = delete;

private:
  friend class ResponseBuilder;
  explicit Writer(xHttpCtx *ctx) : m_ctx(ctx) {}
  xHttpCtx *m_ctx;
};

inline void ResponseBuilder::stream(std::function<void(Writer &)> fn) {
  Writer w(m_ctx);
  fn(w);
}

// ═════════════════════════════════════════════════════════════════════
// Request — server-side request (method, url, headers, params)
// ═════════════════════════════════════════════════════════════════════

class Request {
public:
  const std::string &method() const {
    return m_method;
  }
  const std::string &path() const {
    return m_path;
  }

  /// Extract a route parameter (e.g. "/users/:id" → param("id") = "42").
  std::string param(const std::string &name) const {
    size_t      len = 0;
    const char *val = xHttpCtxParam(m_ctx, name.c_str(), &len);
    return val ? std::string(val, len) : std::string();
  }

  /// Look up a request header by name.
  Option<std::string> header(const std::string &name) const {
    auto it = m_headers.find(name);
    if (it != m_headers.end()) return Option<std::string>(it->second);
    return none;
  }

  /// Build a response.
  ResponseBuilder respond() {
    return ResponseBuilder(m_ctx);
  }

private:
  friend class Router;
  xHttpCtx                               *m_ctx;
  std::string                             m_method;
  std::string                             m_path;
  std::multimap<std::string, std::string> m_headers;

  Request(xHttpCtx *ctx, const char *method, const char *path)
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
        std::string s(line, raw - line);
        auto        colon = s.find(':');
        if (colon != std::string::npos) {
          std::string key = s.substr(0, colon);
          std::string val = s.substr(colon + 1);
          // trim leading space from value
          if (!val.empty() && val[0] == ' ') val.erase(0, 1);
          m_headers.emplace(std::move(key), std::move(val));
        }
      }
      if (raw < end) ++raw; // skip '\n'
    }
  }
};

// ═════════════════════════════════════════════════════════════════════
// Router
// ═════════════════════════════════════════════════════════════════════

using Handler = std::function<void(Request &)>;

namespace _ {

/// Per-route state stored on the heap. The pointer is passed as `arg`
/// to xHttpMuxHandle and forwarded to the C callback (on_done).
struct RouteState {
  Handler handler;
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
  /// Pattern examples: "/hello", "GET /hello", "POST /echo", "/users/:id".
  Router &route(const std::string &pattern, Handler handler) {
    auto *rs = new _::RouteState(std::move(handler));
    m_routes.push_back(rs);

    xHttpRouteConf conf = {};
    conf.pattern        = pattern.c_str();
    conf.on_done        = on_request;
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

  /// C callback: build Request, invoke Handler, response is written
  /// synchronously before this function returns.
  static void on_request(xHttpCtx *ctx, void *arg) {
    auto   *rs = static_cast<_::RouteState *>(arg);
    Request req(ctx, ctx->method, ctx->url);
    // Parse request headers
    if (ctx->headers && ctx->headers_len) {
      req.parse_headers(ctx->headers, ctx->headers_len);
    }
    rs->handler(req);
  }
};

// ═════════════════════════════════════════════════════════════════════
// Server — wraps xHttpServer lifecycle.
//
//   Server::bind(":8080", router).serve();   // test setup
  // Future: Server::bind(":8080", router).run();  // serve + event loop
//   Server::bind(":8080", router).serve();    // production (TBD)
//
// bind() creates the server (xHttpServerCreate + OwnedHandle),
// listen() calls xHttpServerListen, serve() wraps listen() + event loop.
// ═════════════════════════════════════════════════════════════════════

class Server {
public:
  /// Create the server, attach the router, parse "host:port".
  /// Returns Err(Builder) on invalid address or server creation failure.
  static Result<Server> bind(const char *addr, Router &router);

  Server()                                     = default;
  Server(Server &&) noexcept            = default;
  Server &operator=(Server &&) noexcept = default;

  /// Start listening. Does NOT run the event loop.
  Server &listen();

  /// Access the raw xHttpServer handle (for event loop integration).
  xHttpServer raw() const { return m_server.get(); }

private:
  struct Deleter {
    void deallocate(void *p, Layout) const noexcept {
      if (p) xHttpServerDestroy(static_cast<xHttpServer>(p));
    }
  };
  Server(const Server &)                        = delete;
  Server &operator=(const Server &)             = delete;
  OwnedHandle<Deleter> m_server;
  std::string           m_host;
  uint16_t              m_port = 0;
};

inline Result<Server> Server::bind(const char *addr, Router &router) {
  const char *colon = std::strrchr(addr, ':');
  if (!colon) return Result<Server>(xpp::err, Error::builder("invalid address"));

  Server srv;
  srv.m_host.assign(addr, static_cast<size_t>(colon - addr));
  if (srv.m_host.empty()) srv.m_host = "0.0.0.0";
  srv.m_port = static_cast<uint16_t>(std::strtoul(colon + 1, nullptr, 10));

  xHttpServerConf conf = {};
  conf.resolve  = xHttpMuxResolve;
  conf.router   = router.raw();
  conf.idle_timeout_ms = 60000;

  OwnedHandle<Deleter> h(xHttpServerCreate(&conf));
  if (!h.get()) return Result<Server>(xpp::err, Error::builder("server creation failed"));
  srv.m_server = std::move(h);
  return Result<Server>(xpp::ok, std::move(srv));
}

inline Server &Server::listen() {
  xHttpServerListen(m_server.get(), m_host.c_str(), m_port);
  return *this;
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_SERVER_H
