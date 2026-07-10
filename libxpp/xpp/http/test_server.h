/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * test_server.h - xpp::http::TestServer — in-process HTTP test server.
 *
 * Go httptest-style helper.  Server + client share the same event
 * loop — Client::send() drives curl I/O via the fiber scheduler.
 *
 *   xpp::http::TestServer ts;
 *   ts.router().route("GET /hello", [](IncomingRequest &req) {
 *     return Response::builder().status(200).body(...).into_promise();
 *   });
 *   ts.start();
 *
 *   auto result = ts.get("/hello").await();
 *   auto &resp  = result.unwrap();
 *   EXPECT_EQ(resp.status(), 200);
 */

#ifndef XPP_HTTP_TEST_SERVER_H
#define XPP_HTTP_TEST_SERVER_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <utility>

#include <xpp/http/client.h>
#include <xpp/http/request.h>
#include <xpp/http/server.h>

namespace xpp {
namespace http {

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

  /// GET request.  Returns Promise<Result<Response>> — call .await()
  /// inside a WaitScope to block the fiber until the response arrives.
  auto get(const char *path) {
    auto req = Request::builder().method(Method::Get).url(url(path)).body().unwrap();
    return m_client.send(std::move(req));
  }

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

  Router                m_router;
  uint16_t              m_port   = 0;
  Client                m_client = Client::builder().build();
  Server                m_server;
  Promise<Result<void>> m_shutdown;
  xEventLoop            m_loop   = nullptr;
};

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

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_TEST_SERVER_H
