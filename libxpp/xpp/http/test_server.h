/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * test_server.h - xpp::http::TestServer — in-process HTTP test server.
 *
 * Go httptest-style helper.  Server + client share the same event
 * loop — run_until() drives both accept/handler and curl I/O via
 * the proven C xHttpClient path (avoids PromiseWaker/X_RUN_ONCE
 * re-entrancy issues across test boundaries).
 *
 *   xpp::http::TestServer ts;
 *   ts.router().route("GET /hello", [](IncomingRequest &req) {
 *     return Response::ok("Hello!").into_promise();
 *   });
 *   ts.start();
 *
 *   auto resp = ts.get("/hello");
 *   EXPECT_EQ(resp.status, 200);
 *   EXPECT_EQ(resp.body, "Hello!");
 */

#ifndef XPP_HTTP_TEST_SERVER_H
#define XPP_HTTP_TEST_SERVER_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <utility>

#include <x/base/test_helper.h>
#include <x/http/client.h>

#include <xpp/http/server.h>

namespace xpp {
namespace http {

/// Synchronous result of a TestServer request.
struct TestResponse {
  int         status = 0;
  std::string body;
};

class TestServer {
public:
  TestServer() = default;

  TestServer(const TestServer &)            = delete;
  TestServer &operator=(const TestServer &) = delete;

  // ── Router access ────────────────────────────────────────────────

  Router &router() { return m_router; }

  // ── Lifecycle ────────────────────────────────────────────────────

  /// Start listening on a free port.  Must be inside a WaitScope.
  void start();

  /// Shut down the server.
  void stop();

  // ── URL helpers ──────────────────────────────────────────────────

  std::string url(const char *path) const {
    return "http://127.0.0.1:" + std::to_string(m_port) + path;
  }

  uint16_t port() const { return m_port; }

  // ── Convenience request helpers ──────────────────────────────────

  /// GET request.  Drives the event loop until the response is complete.
  TestResponse get(const char *path);

private:
  static uint16_t find_free_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr = {};
    addr.sin_family         = AF_INET;
    addr.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    addr.sin_port           = 0;
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
      close(fd);
      return 0;
    }
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len);
    uint16_t p = ntohs(addr.sin_port);
    close(fd);
    return p;
  }

  Router                    m_router;
  uint16_t                  m_port = 0;
  Server                    m_server;
  Promise<Result<void>>     m_shutdown;
  xEventLoop                m_loop  = nullptr;
};

// ── Internal helpers for get() ─────────────────────────────────────

namespace _ {
struct GetCtx {
  std::string       body;
  int               status = 0;
  std::atomic<bool> done{false};
};

inline int  get_on_data(const char *data, size_t len, void *arg) {
  static_cast<GetCtx *>(arg)->body.append(data, len);
  return 0;
}

inline void get_on_done(xHttpCtx *ctx, void *arg) {
  auto *c     = static_cast<GetCtx *>(arg);
  c->status   = static_cast<int>(ctx->status_code);
  c->done.store(true, std::memory_order_release);
}
} // namespace _

/* ── TestServer::start ────────────────────────────────────────────── */

inline void TestServer::start() {
  m_loop   = static_cast<xEventLoop>(xEventLoopCurrent());  // NOLINT
  m_port   = find_free_port();
  m_server = Server::bind(("127.0.0.1:" + std::to_string(m_port)).c_str())
               .unwrap();
  m_shutdown = m_server.serve(m_router);
}

/* ── TestServer::stop ─────────────────────────────────────────────── */

inline void TestServer::stop() {
  m_server = Server();
}

/* ── TestServer::get ──────────────────────────────────────────────── */

inline TestResponse TestServer::get(const char *path) {
  _::GetCtx   ctx;
  xHttpClient client = xHttpClientCreate(nullptr);

  auto             url_str = url(path);
  xHttpRequestConf conf    = {};
  conf.url                 = url_str.c_str();
  conf.method              = xHttpMethod_GET;
  conf.on_data             = _::get_on_data;
  conf.on_done             = _::get_on_done;

  xHttpClientDo(client, &conf, &ctx);
  run_until(m_loop, ctx.done);

  xHttpClientDestroy(client);
  return {ctx.status, std::move(ctx.body)};
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_TEST_SERVER_H
