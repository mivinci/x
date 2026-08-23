/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * test_server.h — Minimal HTTP/1.1 test server for xpp::http client tests.
 *
 * A static-response test fixture: bind a loopback xTcpListener on port 0
 * (kernel-assigned), accept connections, read the request (headers + body
 * framed by Content-Length), optionally delay `delay_ms` (to test client
 * timeouts), then write back a preset HTTP/1.1 response and close.
 *
 * Implementation: pure libx C API (xTcpListener + xTcpConn + xSocket +
 * xTimer). No xpp::fiber, no .then() chains. The accept callback switches
 * the conn's xSocket (level-triggered) to a read callback; it accumulates
 * the request until the full header block + body have arrived, then
 * responds (or schedules a timer for delayed responses). This avoids
 * fiber/xEventLoopRun interaction issues on Linux shared builds.
 *
 * NOT the future `xpp::http::Server` module — test-only, no routing,
 * no concurrency, no streaming. Kept in `xpp::http::test` subnamespace
 * and NOT included from the public `xpp/http.h` umbrella.
 */

#ifndef XPP_HTTP_TEST_SERVER_H
#define XPP_HTTP_TEST_SERVER_H

#include <arpa/inet.h> // ntohs, sockaddr_in
#include <errno.h>
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

#include <x/base/base.h>   // XDEF_HANDLE
#include <x/base/event.h>  // xTimerStart
#include <x/base/socket.h> // xSocket, xSocketSetCallback, xSocketSetMask
#include <x/net/tcp.h>     // xTcpListener, xTcpConn, xTcpConnSend, xTcpConnClose

namespace xpp {
namespace http {
namespace test {

/**
 * @brief Preset HTTP response for `TestServer` to return.
 */
struct TestResponseSpec {
  StatusCode::Value              status = StatusCode::Ok;
  Vec<std::pair<String, String>> headers;
  Bytes                          body;
  /** Pre-response delay in milliseconds. 0 = respond immediately. */
  uint64_t delay_ms = 0;
  /**
   * @brief If true, the response body echoes the request body received.
   *
   * Used to verify request bodies are transmitted (e.g. POST payloads).
   * The preset `body` is ignored when this is set.
   */
  bool echo_request_body = false;
  /**
   * @brief If true, the response includes `X-Echo-Method: <request method>`
   *        so tests can verify the client sent the intended HTTP verb.
   */
  bool echo_request_method = false;
  /**
   * @brief If non-empty, redirect requests whose path is not @p redirect_to
   *        to it with a 302 Found + Location header.
   *
   * Used to test client redirect following. The final (target) request
   * is served with the rest of this spec (status/headers/body).
   */
  String redirect_to;
  /**
   * @brief If > 0, send only the first N bytes of `body` but declare the
   *        full Content-Length, then close the connection.
   *
   * Simulates a mid-body disconnect for error-path tests.
   */
  size_t truncate_body_after = 0;
  /**
   * @brief If > 0, stall for this many ms after sending half the response
   *        body, then continue. Simulates a stalled peer for read-timeout
   *        (low-speed) tests.
   */
  uint64_t mid_body_delay_ms = 0;
};

/**
 * @brief Static-response HTTP/1.1 test server.
 *
 * Bind to `127.0.0.1:0` (kernel-assigned port), accept connections on
 * the current `EventLoop`, read each request (headers + Content-Length
 * body), and respond with the preset `TestResponseSpec`. Closes the
 * connection after each response.
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
  xTcpListener     listener_ = nullptr;
  uint16_t         port_     = 0;

  /* ── Per-connection state (heap-allocated; freed once responded) ── */

  struct ConnState {
    TestServer  *self;
    xTcpConn     conn;
    xSocket      sock;               ///< The conn's xSocket (level-triggered source).
    xTimer       timer = nullptr;    ///< Pending delay timer, if any.
    Vec<uint8_t> buf;                ///< Request bytes accumulated so far.
    size_t       header_end     = 0; ///< Index past the "\r\n\r\n" of the header block.
    size_t       content_length = 0; ///< Parsed Content-Length (0 = no body).
    bool         headers_done   = false;
    bool         closed         = false; ///< Guards against double cleanup.
    String       request_path;           ///< Path from the request line (e.g. "/b").
    String       request_method;         ///< Method token (e.g. "GET").
    String       resp;                   ///< Response being sent (send_off marks progress).
    size_t       send_off          = 0;
    uint64_t     mid_body_delay_ms = 0;     ///< From spec — stall after half the body.
    bool         mid_body_paused   = false; ///< The stall already happened.
  };

  /* ── Accept callback (runs on EventLoop thread) ──────────────────── */

  static void on_accept(xTcpListener /*listener*/, xTcpConn conn, const struct sockaddr * /*addr*/,
                        socklen_t /*addrlen*/, void        *arg) {
    auto *self = static_cast<TestServer *>(arg);

    auto *st = new ConnState;
    st->self = self;
    st->conn = conn;

    // Drive reads/writes through the xSocket's own event source (created
    // level-triggered by tcp_listener.c). Never register a second source on
    // the conn fd — re-adding the same (fd, filter) to kqueue keeps the
    // first registration's EV_CLEAR and breaks level-triggered re-firing.
    st->sock              = xTcpConnSocket(conn);
    st->mid_body_delay_ms = self->spec_.mid_body_delay_ms;
    XPP_ASSERT(st->sock != nullptr, "TestServer: xTcpConnSocket failed");
    xSocketSetCallback(st->sock, on_xsocket_event, st);
    xSocketSetMask(st->sock, xEvent_Read);
  }

  /* ── Conn event callback (runs on EventLoop thread) ─────────────── */

  static void on_xsocket_event(xSocket sock, xEventMask mask, void *arg) {
    auto *st = static_cast<ConnState *>(arg);
    (void)sock;
    if (mask & xEvent_Read) on_conn_readable(st);
    if (mask & xEvent_Write) pump_send(st);
  }

  static void on_conn_readable(ConnState *st) {
    char    tmp[4096];
    ssize_t n = xTcpConnRecv(st->conn, tmp, sizeof(tmp));
    if (n <= 0) {
      close_conn(st); // EOF or error — client gone before we responded
      return;
    }
    for (ssize_t i = 0; i < n; ++i)
      st->buf.push(static_cast<uint8_t>(tmp[i]));

    if (!st->headers_done) {
      size_t hs = find_header_end(st->buf);
      if (hs != SIZE_MAX) {
        st->header_end     = hs;
        st->content_length = parse_content_length(st->buf, hs);
        st->request_path   = parse_request_path(st->buf, hs);
        st->request_method = parse_request_method(st->buf);
        st->headers_done   = true;
      }
    }

    // Respond once the header block and the full body have arrived.
    if (st->headers_done && st->buf.len() >= st->header_end + st->content_length) {
      if (st->self->spec_.delay_ms > 0) {
        // Delayed response — schedule a one-shot timer. The timer owns the
        // ConnState (arg) until it fires or the loop is destroyed.
        st->timer = xTimerStart(on_delay_timer, st, on_delay_cancel, st->self->spec_.delay_ms, 0);
        XPP_ASSERT(st->timer != nullptr, "TestServer: xTimerStart failed");
      } else {
        respond_and_close(st);
      }
    }
  }

  /* ── Response path ──────────────────────────────────────────────── */

  static void on_delay_timer(void *arg) {
    auto *st  = static_cast<ConnState *>(arg);
    st->timer = nullptr; // handle consumed by the fire — don't xTimerStop it
    respond_and_close(st);
  }

  static void on_delay_cancel(void *arg) {
    auto *st  = static_cast<ConnState *>(arg);
    st->timer = nullptr; // handle consumed by the cancel
    close_conn(st);
  }

  static void respond_and_close(ConnState *st) {
    // Build the response, then switch the source to write mode and pump
    // the send. The socket is non-blocking — a single xTcpConnSend can
    // write only part of a large response, so we drive the rest from
    // level-triggered writable events.
    st->resp     = build_response(*st);
    st->send_off = 0;
    xSocketSetMask(st->sock, xEvent_Write);
    pump_send(st);
  }

  static void on_mid_body_timer(void *arg) {
    auto *st  = static_cast<ConnState *>(arg);
    st->timer = nullptr;                    // handle consumed by the fire
    xSocketSetMask(st->sock, xEvent_Write); // re-arm writable, resume sending
    pump_send(st);
  }

  static void on_mid_body_cancel(void *arg) {
    auto *st  = static_cast<ConnState *>(arg);
    st->timer = nullptr; // handle consumed by the cancel
    close_conn(st);
  }

  static void pump_send(ConnState *st) {
    // Mid-body stall (read-timeout testing): after half the response is
    // out, pause once for mid_body_delay_ms before continuing.
    if (st->mid_body_delay_ms > 0 && !st->mid_body_paused && st->send_off >= st->resp.len() / 2) {
      st->mid_body_paused = true;
      // Unregister the writable event so the stall actually blocks sending
      // (a writable event would otherwise fire as soon as the client reads).
      xSocketSetMask(st->sock, 0);
      st->timer = xTimerStart(on_mid_body_timer, st, on_mid_body_cancel, st->mid_body_delay_ms, 0);
      XPP_ASSERT(st->timer != nullptr, "TestServer: xTimerStart failed");
      return;
    }
    auto bytes = st->resp.as_bytes();
    // Limit per-send size: without this, a loopback socket buffer can
    // swallow the whole response before the mid-body stall, making the
    // pause invisible to the client.
    size_t remaining = bytes.size() - st->send_off;
    if (remaining > 64 * 1024) remaining = 64 * 1024;
    ssize_t n = xTcpConnSend(st->conn, reinterpret_cast<const char *>(bytes.data()) + st->send_off,
                             remaining);
    if (n < 0) {
      // Non-blocking socket: EAGAIN means "retry when writable" — the
      // level-triggered source fires again. Anything else is fatal.
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      close_conn(st);
      return;
    }
    if (n == 0) { // defensive: no progress — give up rather than spin
      close_conn(st);
      return;
    }
    st->send_off += static_cast<size_t>(n);
    if (st->send_off >= bytes.size()) close_conn(st);
    // else: partial write — the level-triggered writable event re-fires.
  }

  static void close_conn(ConnState *st) {
    if (st->closed) return;
    st->closed = true;
    // Cancel a still-pending delay timer (EOF arrived before it fired).
    // Fired/cancelled timers already set st->timer = nullptr.
    if (st->timer) {
      xTimerStop(st->timer);
      st->timer = nullptr;
    }
    // xTcpConnClose destroys the xSocket (and its event source).
    xTcpConnClose(st->conn);
    delete st;
  }

  /* ── Request parsing ────────────────────────────────────────────── */

  /** @brief Index past the "\r\n\r\n" ending the header block, or SIZE_MAX. */
  static size_t find_header_end(const Vec<uint8_t> &buf) {
    size_t n = buf.len();
    if (n < 4) return SIZE_MAX;
    for (size_t i = 0; i + 4 <= n; ++i) {
      if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
        return i + 4;
      }
    }
    return SIZE_MAX;
  }

  /** @brief Request path from the request line, e.g. "/b" from "GET /b HTTP/1.1". */
  static String parse_request_path(const Vec<uint8_t> &buf, size_t header_end) {
    const uint8_t *p = buf.data();
    size_t         i = 0;
    while (i < header_end && p[i] != ' ' && p[i] != '\t')
      ++i; // skip method token
    while (i < header_end && (p[i] == ' ' || p[i] == '\t'))
      ++i; // skip whitespace
    size_t start = i;
    while (i < header_end && p[i] != ' ' && p[i] != '\t' && p[i] != '\r')
      ++i;
    if (i == start) return String();
    return String::from_utf8(reinterpret_cast<const char *>(p + start), i - start).unwrap();
  }

  /** @brief Method token from the request line, e.g. "GET" from "GET /b HTTP/1.1". */
  static String parse_request_method(const Vec<uint8_t> &buf) {
    const uint8_t *p = buf.data();
    size_t         i = 0;
    while (i < buf.len() && p[i] != ' ' && p[i] != '\t' && p[i] != '\r')
      ++i;
    return String::from_utf8(reinterpret_cast<const char *>(p), i).unwrap();
  }

  /** @brief Parse Content-Length from the header block. 0 if absent. */
  static size_t parse_content_length(const Vec<uint8_t> &buf, size_t header_end) {
    static const char kName[] = "content-length";
    const uint8_t    *p       = buf.data();
    size_t            i       = 0;
    while (i < header_end) {
      size_t le = i;
      while (le < header_end && p[le] != '\n')
        ++le;
      size_t len = le;
      if (len > i && p[len - 1] == '\r') --len;

      size_t j = 0;
      for (; j < sizeof(kName) - 1 && i + j < len; ++j) {
        uint8_t c = p[i + j];
        if (c >= 'A' && c <= 'Z') c = static_cast<uint8_t>(c - 'A' + 'a');
        if (c != static_cast<uint8_t>(kName[j])) break;
      }
      if (j == sizeof(kName) - 1 && i + j < len && p[i + j] == ':') {
        size_t d = i + j + 1;
        while (d < len && (p[d] == ' ' || p[d] == '\t'))
          ++d;
        size_t value = 0;
        while (d < len && p[d] >= '0' && p[d] <= '9') {
          value = value * 10 + static_cast<size_t>(p[d] - '0');
          ++d;
        }
        return value;
      }
      if (le >= header_end) break;
      i = le + 1;
    }
    return 0;
  }

  /* ── Response building ──────────────────────────────────────────── */

  static String build_response(const ConnState &st) {
    const TestResponseSpec &spec = st.self->spec_;
    String                  resp;

    // Redirect: 302 + Location for any request not already at the target.
    // Lets the client exercise CURLOPT_FOLLOWLOCATION end to end.
    if (!spec.redirect_to.empty() && st.request_path != spec.redirect_to) {
      resp.push_str(String::from_utf8("HTTP/1.1 302 Found\r\nLocation: ").unwrap());
      resp.push_str(spec.redirect_to);
      resp.push_str(
        String::from_utf8("\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").unwrap());
      return resp;
    }

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

    if (spec.echo_request_method) {
      resp.push_str(String::from_utf8("X-Echo-Method: ").unwrap());
      resp.push_str(st.request_method);
      resp.push_str(String::from_utf8("\r\n").unwrap());
    }

    // Body: echoed request body or the preset spec body.
    Bytes body = spec.body;
    if (spec.echo_request_body) {
      size_t blen = st.buf.len() - st.header_end;
      body = Bytes::copy(reinterpret_cast<const char *>(st.buf.data() + st.header_end), blen);
    }
    // Truncation: declare the full length, send only the first N bytes.
    size_t declared_len = body.size();
    if (spec.truncate_body_after > 0 && body.size() > spec.truncate_body_after) {
      body = Bytes::copy(reinterpret_cast<const char *>(body.data()), spec.truncate_body_after);
    }

    // Content-Length + Connection: close
    resp.push_str(String::from_utf8("Content-Length: ").unwrap());
    resp.push_str(String::from_utf8(std::to_string(declared_len).c_str()).unwrap());
    resp.push_str(String::from_utf8("\r\n").unwrap());
    resp.push_str(String::from_utf8("Connection: close\r\n\r\n").unwrap());

    // Body (appended to the same buffer for a single send)
    if (body.size() > 0) {
      auto body_bytes = body.as_span();
      // String is UTF-8 — body may be binary, but for test purposes
      // appending raw bytes works because String stores Vec<uint8_t>.
      resp.push_str(
        String::from_utf8(reinterpret_cast<const char *>(body_bytes.data()), body_bytes.size())
          .unwrap_or(String()));
    }
    return resp;
  }

  /** @brief Minimal reason-phrase table for common status codes. */
  static String to_reason_phrase(StatusCode::Value code) {
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
