/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * test_server.h — Minimal HTTP/1.1 test server for xpp::http client tests.
 *
 * A static-response test fixture: bind a loopback xTcpListener on port 0
 * (kernel-assigned), accept connections, optionally delay `delay_ms`
 * (to test client timeouts), then write back a preset HTTP/1.1 response
 * and close.
 *
 * Implementation: pure libx C API (xTcpListener + xTcpConn + xTimer).
 * No xpp::fiber, no .then() chains. The accept callback runs on the
 * EventLoop thread and synchronously writes the response (or schedules
 * a timer for delayed responses). This avoids fiber/xEventLoopRun
 * interaction issues on Linux shared builds.
 *
 * NOT the future `xpp::http::Server` module — test-only, no routing,
 * no concurrency, no streaming. Kept in `xpp::http::test` subnamespace
 * and NOT included from the public `xpp/http.h` umbrella.
 */

#ifndef XPP_HTTP_TEST_SERVER_H
#define XPP_HTTP_TEST_SERVER_H

#include <arpa/inet.h>  // ntohs, sockaddr_in
#include <sys/socket.h> // getsockname, sockaddr_storage

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <xpp/bytes.h>
#include <xpp/http/status.h>
#include <xpp/string.h>
#include <xpp/vec.h>

#include <x/base/base.h>  // XDEF_HANDLE
#include <x/base/event.h> // xTimerStart, xEventLoopCurrent
#include <x/net/tcp.h>    // xTcpListener, xTcpConn, xTcpConnSend, xTcpConnClose

namespace xpp {
namespace http {
namespace test {

/**
 * @brief Preset HTTP response for `TestServer` to return.
 */
struct TestResponseSpec {
  StatusCode                     status = StatusCode::Ok;
  Vec<std::pair<String, String>> headers;
  Bytes                          body;
  /** Pre-response delay in milliseconds. 0 = respond immediately. */
  uint64_t delay_ms = 0;
};

/**
 * @brief Static-response HTTP/1.1 test server.
 *
 * Bind to `127.0.0.1:0` (kernel-assigned port), accept connections on
 * the current `EventLoop`, and respond to every request with the same
 * preset `TestResponseSpec`. Closes the connection after each response.
 *
 * Usage:
 * @code
 *   xpp::EventLoop loop;
 *   xpp::WaitScope scope(loop);
 *
 *   xpp::http::test::TestResponseSpec spec;
 *   spec.status = xpp::http::StatusCode::Ok;
 *   spec.body   = xpp::Bytes::from("hello");
 *
 *   auto server = xpp::http::test::TestServer::start(spec);
 *   // server.port() → kernel-assigned port
 * @endcode
 */
class TestServer {
public:
  TestServer()                                  = default;
  TestServer(TestServer &&) noexcept            = default;
  TestServer &operator=(TestServer &&) noexcept = default;
  TestServer(const TestServer &)                = delete;
  TestServer &operator=(const TestServer &)     = delete;

  ~TestServer() {
    stop();
  }

  /**
   * @brief Start a `TestServer` bound to `127.0.0.1:0`.
   *
   * Must be called inside a `WaitScope` (the current thread must have
   * entered an `EventLoop`). Returns a move-only `TestServer` value.
   */
  static TestServer start(TestResponseSpec spec) {
    TestServer ts;
    ts.spec_ = std::move(spec);

    // Build the response bytes once — reused for every connection.
    ts.response_buf_ = build_response(ts.spec_);

    // Create the listener. xTcpListenerCreate uses xEventLoopCurrent(),
    // so the caller must have entered an EventLoop.
    ts.listener_ = xTcpListenerCreate("127.0.0.1", 0, nullptr, on_accept, &ts);
    XPP_ASSERT(ts.listener_ != nullptr, "TestServer: xTcpListenerCreate failed");

    // Get the kernel-assigned port.
    xSocket sock = xTcpListenerSocket(ts.listener_);
    XPP_ASSERT(sock != nullptr, "TestServer: xTcpListenerSocket failed");

    struct sockaddr_storage addr;
    socklen_t               addrlen = sizeof(addr);
    if (getsockname(xSocketFd(sock), (struct sockaddr *)&addr, &addrlen) == 0) {
      ts.port_ = ntohs(((struct sockaddr_in *)&addr)->sin_port);
    }
    XPP_ASSERT(ts.port_ > 0, "TestServer: getsockname failed");

    return ts;
  }

  /** @brief The kernel-assigned port number. */
  uint16_t port() const noexcept {
    return port_;
  }

  /**
   * @brief Stop the server.
   *
   * Closes the listening socket. In-flight connections are not affected.
   * Idempotent.
   */
  void stop() {
    if (listener_) {
      xTcpListenerDestroy(listener_);
      listener_ = nullptr;
    }
  }

private:
  TestResponseSpec spec_;
  String           response_buf_;
  xTcpListener     listener_ = nullptr;
  uint16_t         port_     = 0;

  /* ── Build the HTTP/1.1 response bytes ─────────────────────────── */

  static String build_response(const TestResponseSpec &spec) {
    String resp;
    // Status line
    resp.push_str(String::from_utf8("HTTP/1.1 ").unwrap());
    resp.push_str(
      String::from_utf8(std::to_string(static_cast<uint16_t>(spec.status)).c_str()).unwrap());
    resp.push_str(String::from_utf8(" ").unwrap());
    resp.push_str(to_reason_phrase(spec.status));
    resp.push_str(String::from_utf8("\r\n").unwrap());

    // User-provided headers
    for (const auto &h : spec.headers) {
      resp.push_str(h.first);
      resp.push_str(String::from_utf8(": ").unwrap());
      resp.push_str(h.second);
      resp.push_str(String::from_utf8("\r\n").unwrap());
    }

    // Content-Length + Connection: close
    resp.push_str(String::from_utf8("Content-Length: ").unwrap());
    resp.push_str(String::from_utf8(std::to_string(spec.body.size()).c_str()).unwrap());
    resp.push_str(String::from_utf8("\r\n").unwrap());
    resp.push_str(String::from_utf8("Connection: close\r\n\r\n").unwrap());

    // Body (appended to the same buffer for a single send)
    if (spec.body.size() > 0) {
      auto body_bytes = spec.body.as_span();
      // String is UTF-8 — body may be binary, but for test purposes
      // appending raw bytes works because String stores Vec<uint8_t>.
      // We use a const-char* cast since xTcpConnSend takes const char*.
      resp.push_str(
        String::from_utf8(reinterpret_cast<const char *>(body_bytes.data()), body_bytes.size())
          .unwrap_or(String()));
    }
    return resp;
  }

  /* ── Accept callback (runs on EventLoop thread) ────────────────── */

  static void on_accept(xTcpListener /*listener*/, xTcpConn conn, const struct sockaddr * /*addr*/,
                        socklen_t /*addrlen*/, void        *arg) {
    auto *self = static_cast<TestServer *>(arg);

    if (self->spec_.delay_ms > 0) {
      // Delayed response — schedule a timer.
      // The timer callback writes the response and closes the connection.
      // We allocate the args on the heap; the timer callback frees them.
      auto  *delay_arg = new DelayArg{self, conn};
      xTimer timer =
        xTimerStart(on_delay_timer, delay_arg, on_delay_cancel, self->spec_.delay_ms, 0);
      (void)timer; // timer handle is owned by the event loop; it fires once
    } else {
      // Immediate response — write synchronously and close.
      send_response(self, conn);
      xTcpConnClose(conn);
    }
  }

  struct DelayArg {
    TestServer *self;
    xTcpConn    conn;
  };

  static void on_delay_timer(void *arg) {
    auto *da = static_cast<DelayArg *>(arg);
    send_response(da->self, da->conn);
    xTcpConnClose(da->conn);
    delete da;
  }

  static void on_delay_cancel(void *arg) {
    auto *da = static_cast<DelayArg *>(arg);
    xTcpConnClose(da->conn);
    delete da;
  }

  static void send_response(const TestServer *self, xTcpConn conn) {
    auto bytes = self->response_buf_.as_bytes();
    xTcpConnSend(conn, reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }

  /** @brief Minimal reason-phrase table for common status codes. */
  static String to_reason_phrase(StatusCode code) {
    switch (code) {
    case StatusCode::Ok:
      return String::from_utf8("OK").unwrap();
    case StatusCode::Created:
      return String::from_utf8("Created").unwrap();
    case StatusCode::NoContent:
      return String::from_utf8("No Content").unwrap();
    case StatusCode::MovedPermanently:
      return String::from_utf8("Moved Permanently").unwrap();
    case StatusCode::Found:
      return String::from_utf8("Found").unwrap();
    case StatusCode::NotModified:
      return String::from_utf8("Not Modified").unwrap();
    case StatusCode::BadRequest:
      return String::from_utf8("Bad Request").unwrap();
    case StatusCode::Unauthorized:
      return String::from_utf8("Unauthorized").unwrap();
    case StatusCode::Forbidden:
      return String::from_utf8("Forbidden").unwrap();
    case StatusCode::NotFound:
      return String::from_utf8("Not Found").unwrap();
    case StatusCode::MethodNotAllowed:
      return String::from_utf8("Method Not Allowed").unwrap();
    case StatusCode::InternalServerError:
      return String::from_utf8("Internal Server Error").unwrap();
    case StatusCode::NotImplemented:
      return String::from_utf8("Not Implemented").unwrap();
    case StatusCode::BadGateway:
      return String::from_utf8("Bad Gateway").unwrap();
    case StatusCode::ServiceUnavailable:
      return String::from_utf8("Service Unavailable").unwrap();
    case StatusCode::GatewayTimeout:
      return String::from_utf8("Gateway Timeout").unwrap();
    default:
      return String::from_utf8("OK").unwrap();
    }
  }
};

} // namespace test
} // namespace http
} // namespace xpp

#endif // XPP_HTTP_TEST_SERVER_H
