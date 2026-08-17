/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * test_server.h — Minimal HTTP/1.1 test server for xpp::http client tests.
 *
 * A static-response test fixture: bind a loopback TcpListener on port 0
 * (kernel-assigned), accept connections, read each request up to the
 * blank-line terminator (\r\n\r\n), optionally drain the request body
 * (Content-Length), optionally sleep `delay_ms` (to test client timeouts),
 * then write back a preset HTTP/1.1 response and close.
 *
 * NOT the future `xpp::http::Server` module — test-only, no routing,
 * no concurrency limit, no streaming. Kept in `xpp::http::test`
 * subnamespace and NOT included from the public `xpp/http.h` umbrella.
 */

#ifndef XPP_HTTP_TEST_SERVER_H
#define XPP_HTTP_TEST_SERVER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>  // std::shared_ptr, std::make_shared
#include <strings.h>  // strncasecmp
#include <utility>

#include <xpp/bytes.h>
#include <xpp/fiber.h>
#include <xpp/http/status.h>
#include <xpp/net/addr.h>
#include <xpp/net/tcp.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/shared.h>
#include <xpp/string.h>
#include <xpp/vec.h>

namespace xpp {
namespace http {
namespace test {

using xpp::net::TcpListener;
using xpp::net::TcpStream;
using xpp::net::SocketAddr;
using xpp::net::SocketAddrV4;
using xpp::net::Ipv4Addr;

/**
 * @brief Preset HTTP response for `TestServer` to return.
 *
 * All fields are value-initialized — a default-constructed `TestResponseSpec`
 * produces a `200 OK` with empty body and no delay.
 */
struct TestResponseSpec {
  StatusCode                             status   = StatusCode::Ok;
  Vec<std::pair<String, String>>         headers;
  Bytes                                 body;
  /** Pre-response delay in milliseconds. 0 = respond immediately. */
  uint64_t                              delay_ms = 0;
};

/**
 * @brief Static-response HTTP/1.1 test server.
 *
 * Bind to `127.0.0.1:0` (kernel-assigned port), accept connections on
 * the current `EventLoop`, and respond to every request with the same
 * preset `TestResponseSpec`. Closes the connection after each response
 * (no keep-alive).
 *
 * Usage:
 * @code
 *   xpp::EventLoop loop;
 *   xpp::WaitScope scope(loop);
 *
 *   xpp::http::test::TestResponseSpec spec;
 *   spec.status = xpp::http::StatusCode::Ok;
 *   spec.headers.push_back({String::from_utf8("Content-Type").unwrap(),
 *                           String::from_utf8("text/plain").unwrap()});
 *   spec.body   = xpp::Bytes::from("hello");
 *
 *   auto server = xpp::http::test::TestServer::start(spec);
 *   // server.port() → kernel-assigned port
 * @endcode
 *
 * The server runs on the caller's `EventLoop` — no background thread.
 * `stop()` closes the listener and unblocks pending accepts; the
 * destructor calls `stop()`.
 */
class TestServer {
public:
  TestServer()                              = default;
  TestServer(TestServer &&) noexcept        = default;
  TestServer &operator=(TestServer &&) noexcept = default;
  TestServer(const TestServer &)            = delete;
  TestServer &operator=(const TestServer &) = delete;

  ~TestServer() {
    stop();
  }

  /**
   * @brief Start a `TestServer` bound to `127.0.0.1:0`.
   *
   * Must be called inside a `WaitScope` (uses `.await()` for the
   * synchronous `bind()`). Returns a move-only `TestServer` value.
   */
  static TestServer start(TestResponseSpec spec) {
    TestServer ts;
    ts.state_ = Shared<State>::make();
    State *s = ts.state_.as_deref();
    XPP_ASSERT(s != nullptr, "TestServer: state alloc failed");
    s->spec = std::move(spec);

    // Bind to loopback port 0 — kernel assigns a free port.
    auto addr = SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::localhost(), 0));
    auto bind_r = TcpListener::bind(addr).await();
    XPP_ASSERT(bind_r.is_ok(), "TestServer: bind failed");
    s->listener = std::move(bind_r).unwrap();

    auto local = s->listener.local_addr();
    XPP_ASSERT(local.is_some(), "TestServer: local_addr failed");
    ts.port_ = local.unwrap().port();

    // Start the accept loop in a fiber. The fiber's `.await()` calls
    // inside the loop register wakers with the EventLoop, so accepts
    // actually fire. The fiber's outer Promise is held in `state->accept_fiber`
    // so it doesn't get detached and lose its resolver before the first
    // accept resolves.
    s->accept_fiber = xpp::fiber(64 * 1024, [state_opt = ts.state_]() {
      accept_loop(state_opt).await();
    });

    return ts;
  }

  /** @brief The kernel-assigned port number. */
  uint16_t port() const noexcept {
    return port_;
  }

  /**
   * @brief Stop the server.
   *
   * Closes the listening socket. Pending accept promises will never
   * resolve — the then-chain simply stops scheduling new accepts.
   * In-flight connection fibers continue until they finish or the
   * `EventLoop` exits.
   *
   * Idempotent.
   */
  void stop() {
    if (state_) {
      State *s = state_.as_deref();
      if (s) {
        s->running.store(false, std::memory_order_release);
        // Closing the listener causes `accept()` to return immediately
        // with a closed TcpStream — the accept_loop fiber sees this
        // and exits.
        s->listener = TcpListener();
      }
    }
  }

private:
  struct State {
    TcpListener        listener;
    TestResponseSpec    spec;
    std::atomic<bool>   running{true};
    // Holds the accept-loop fiber promise. Keeps the fiber's resolver
    // alive so its internal `.await()` calls can be woken.
    Promise<void>       accept_fiber;
  };

  Option<Shared<State>>   state_;
  uint16_t                port_ = 0;

  /* ── Accept loop (then-chain, no fiber) ─────────────────────────── */

  static Promise<void> accept_loop(Option<Shared<State>> state_opt) {
    while (true) {
      State *s = state_opt.as_deref();
      if (!s || !s->running.load(std::memory_order_acquire)) {
        return xpp::resolve();
      }

      // Await next connection. This `.await()` is what registers the
      // waker with the EventLoop — without it, the accept promise never
      // resolves even when a connection arrives.
      auto accepted = s->listener.accept().await();
      if (!accepted.first.is_open()) {
        // Listener closed.
        return xpp::resolve();
      }

      // Copy spec out of shared state before spawning the fiber —
      // the fiber may outlive `state_opt`'s refcount if TestServer
      // is dropped while a connection is mid-flight.
      TestResponseSpec spec_copy = s->spec;

      // Spawn a fiber for this connection. Fire-and-forget: the fiber
      // owns its TcpStream and runs to completion (or until EventLoop exit).
      // We wrap move-only types in shared_ptr because C++11 lambdas
      // can't init-capture by move.
      auto stream_ptr = std::make_shared<TcpStream>(std::move(accepted.first));
      auto spec_ptr   = std::make_shared<TestResponseSpec>(std::move(spec_copy));
      xpp::fiber(32 * 1024, [stream_ptr, spec_ptr]() -> Promise<void> {
        TcpStream       stream = std::move(*stream_ptr);
        TestResponseSpec spec   = std::move(*spec_ptr);
        return handle_connection(std::move(stream), std::move(spec));
      });
    }
  }

  /* ── Per-connection handling (runs inside a fiber) ─────────────── */

  static Promise<void> handle_connection(TcpStream stream, TestResponseSpec spec) {
    // 1. Read request line + headers up to \r\n\r\n.
    //    Buffer is large enough for typical request headers; if the
    //    headers don't fit, we still respond (best-effort).
    static const size_t kBufSize = 8192;
    char  buf[kBufSize];
    size_t total = 0;
    size_t header_end = 0; // index of the byte after the final \n
    bool   headers_done = false;

    while (total < kBufSize && !headers_done) {
      ssize_t n = stream.read(buf + total, kBufSize - total).await();
      if (n <= 0) {
        // Client closed or error — give up.
        return xpp::resolve();
      }
      total += n;

      // Search for \r\n\r\n in the newly-received data.
      // Start from the earliest position where the terminator could
      // begin (overlap with previously-received bytes).
      size_t search_start = (total > n + 3) ? (total - n - 3) : 0;
      for (size_t i = search_start; i + 3 < total; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n'
            && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
          header_end = i + 4;
          headers_done = true;
          break;
        }
      }
    }

    if (!headers_done) {
      // Headers didn't fit in kBufSize — still try to respond.
      header_end = total;
    }

    // 2. Parse Content-Length (case-insensitive) and drain request body.
    size_t content_length = 0;
    bool   has_content_length = false;
    for (size_t i = 0; i + 15 < header_end; i++) {
      if (strncasecmp(buf + i, "Content-Length:", 15) == 0) {
        // Skip whitespace after the colon.
        size_t j = i + 15;
        while (j < header_end && (buf[j] == ' ' || buf[j] == '\t')) j++;
        content_length = static_cast<size_t>(strtoul(buf + j, nullptr, 10));
        has_content_length = true;
        break;
      }
    }

    if (has_content_length) {
      // Body bytes already in buf: total - header_end
      size_t body_received = (total > header_end) ? (total - header_end) : 0;
      size_t remaining = (content_length > body_received)
                           ? (content_length - body_received)
                           : 0;

      // Drain remaining body bytes from the socket.
      char drain[1024];
      while (remaining > 0) {
        size_t to_read = (remaining < sizeof(drain)) ? remaining : sizeof(drain);
        ssize_t n = stream.read(drain, to_read).await();
        if (n <= 0) break; // short read — give up draining
        remaining -= static_cast<size_t>(n);
      }
    }

    // 3. Optional pre-response delay (for timeout tests).
    if (spec.delay_ms > 0) {
      xpp::after(spec.delay_ms).await();
    }

    // 4. Build HTTP/1.1 response bytes.
    //    Format: "HTTP/1.1 <code> <reason>\r\n"
    //            "<Header>: <Value>\r\n"...
    //            "Content-Length: <n>\r\n"
    //            "Connection: close\r\n"
    //            "\r\n"
    //            <body>
    String resp;
    // Status line
    resp.push_str(String::from_utf8("HTTP/1.1 ").unwrap());
    resp.push_str(String::from_utf8(
      std::to_string(static_cast<uint16_t>(spec.status)).c_str()).unwrap());
    resp.push_str(String::from_utf8(" ").unwrap());
    resp.push_str(to_reason_phrase(spec.status));
    resp.push_str(String::from_utf8("\r\n").unwrap());

    // User-provided headers (verbatim)
    for (const auto &h : spec.headers) {
      resp.push_str(h.first);
      resp.push_str(String::from_utf8(": ").unwrap());
      resp.push_str(h.second);
      resp.push_str(String::from_utf8("\r\n").unwrap());
    }

    // Always set Content-Length and Connection: close so the client
    // knows the response boundary.
    resp.push_str(String::from_utf8("Content-Length: ").unwrap());
    resp.push_str(String::from_utf8(
      std::to_string(spec.body.size()).c_str()).unwrap());
    resp.push_str(String::from_utf8("\r\n").unwrap());
    resp.push_str(String::from_utf8("Connection: close\r\n\r\n").unwrap());

    // 5. Write response headers + body in one or more write() calls.
    //    We write headers first, then body — simpler than building
    //    a single contiguous buffer (body may be large).
    const uint8_t *header_data = resp.as_bytes().data();
    size_t         header_len  = resp.as_bytes().size();

    size_t sent = 0;
    while (sent < header_len) {
      ssize_t n = stream.write(header_data + sent, header_len - sent).await();
      if (n <= 0) return xpp::resolve(); // write error — give up
      sent += static_cast<size_t>(n);
    }

    // Body
    if (spec.body.size() > 0) {
      const uint8_t *body_data = spec.body.data();
      size_t         body_len  = spec.body.size();
      size_t         body_sent = 0;
      while (body_sent < body_len) {
        ssize_t n = stream.write(body_data + body_sent, body_len - body_sent).await();
        if (n <= 0) break;
        body_sent += static_cast<size_t>(n);
      }
    }

    // Connection closes when `stream` goes out of scope (fiber exit).
    return xpp::resolve();
  }

  /** @brief Minimal reason-phrase table for common status codes. */
  static String to_reason_phrase(StatusCode code) {
    switch (code) {
      case StatusCode::Ok:                  return String::from_utf8("OK").unwrap();
      case StatusCode::Created:              return String::from_utf8("Created").unwrap();
      case StatusCode::NoContent:            return String::from_utf8("No Content").unwrap();
      case StatusCode::MovedPermanently:     return String::from_utf8("Moved Permanently").unwrap();
      case StatusCode::Found:                return String::from_utf8("Found").unwrap();
      case StatusCode::NotModified:          return String::from_utf8("Not Modified").unwrap();
      case StatusCode::BadRequest:           return String::from_utf8("Bad Request").unwrap();
      case StatusCode::Unauthorized:        return String::from_utf8("Unauthorized").unwrap();
      case StatusCode::Forbidden:           return String::from_utf8("Forbidden").unwrap();
      case StatusCode::NotFound:             return String::from_utf8("Not Found").unwrap();
      case StatusCode::MethodNotAllowed:     return String::from_utf8("Method Not Allowed").unwrap();
      case StatusCode::InternalServerError:  return String::from_utf8("Internal Server Error").unwrap();
      case StatusCode::NotImplemented:       return String::from_utf8("Not Implemented").unwrap();
      case StatusCode::BadGateway:           return String::from_utf8("Bad Gateway").unwrap();
      case StatusCode::ServiceUnavailable:  return String::from_utf8("Service Unavailable").unwrap();
      case StatusCode::GatewayTimeout:       return String::from_utf8("Gateway Timeout").unwrap();
    }
    return String::from_utf8("OK").unwrap();
  }
};

} // namespace test
} // namespace http
} // namespace xpp

#endif // XPP_HTTP_TEST_SERVER_H
