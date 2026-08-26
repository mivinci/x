/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * evil_server.h — xpp::http::test::EvilServer: fault-injection test peer.
 *
 * Layer 2 of the three-layer HTTP testing model: behaviors a
 * well-formed server cannot produce (a server that lies about
 * Content-Length and truncates the body — the wiremock Fault family).
 *
 * Raw TCP (no HTTP library) — the whole point is malformed framing.
 * Construction starts listening on 127.0.0.1:0; RAII destructor closes.
 *
 * Test-only. NOT part of any public umbrella.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_HTTP_TEST_EVIL_SERVER_H
#define XPP_HTTP_TEST_EVIL_SERVER_H

#include <arpa/inet.h>  // ntohs, sockaddr_in
#include <sys/socket.h> // getsockname, sockaddr_storage
#include <unistd.h>     // close

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <xpp/bytes.h>

#include <x/base/event.h>  // xEventLoopEnter/Leave, xEventLoopCurrent
#include <x/base/socket.h> // xSocket, xSocketSetCallback, xSocketSetMask
#include <x/net/tcp.h>     // xTcpListener, xTcpConn, xTcpConnSend, xTcpConnClose

namespace xpp {
namespace http {
namespace test {

/**
 * @brief Preset malicious response.
 */
struct EvilSpec {
  /// The full body — used ONLY to declare Content-Length.
  Bytes body;
  /// Actually send only the first N bytes, then hard-close.
  /// Must be < body.size() to have any effect.
  size_t send_only = 0;
};

/**
 * @brief Fault-injection HTTP peer (raw TCP).
 *
 * Accepts a connection, reads the request (discarded), then writes a
 * response declaring the FULL Content-Length but sending only
 * `send_only` bytes before closing the connection — simulating a
 * mid-body disconnect. The client under test must surface this as a
 * transport error, not a truncated EOF.
 *
 * @code
 *   EvilSpec spec;
 *   spec.body = Bytes::from(std::string(1024 * 1024, 'z')); // 1MB declared
 *   spec.send_only = 64 * 1024;                              // 64KB sent
 *   test::EvilServer evil(spec);
 *   auto r = client.get(url_str(evil.port())).await();
 *   EXPECT_TRUE(r.is_err()); // mid-body failure, not truncated success
 * @endcode
 */
class EvilServer {
public:
  EvilServer()                                  = default;
  EvilServer(EvilServer &&) noexcept            = default;
  EvilServer &operator=(EvilServer &&) noexcept = default;
  EvilServer(const EvilServer &)                = delete;
  EvilServer &operator=(const EvilServer &)     = delete;

  ~EvilServer() {
    close();
  }

  explicit EvilServer(EvilSpec spec) : m_spec(std::move(spec)) {
    // Must be inside a WaitScope (the listener uses xEventLoopCurrent()).
    m_listener = xTcpListenerCreate("127.0.0.1", 0, nullptr, on_accept, this);
    XPP_ASSERT(m_listener != nullptr, "EvilServer: xTcpListenerCreate failed");

    xSocket sock = xTcpListenerSocket(m_listener);
    XPP_ASSERT(sock != nullptr, "EvilServer: xTcpListenerSocket failed");

    struct sockaddr_storage addr;
    socklen_t               addrlen = sizeof(addr);
    if (getsockname(xSocketFd(sock), (struct sockaddr *)&addr, &addrlen) == 0) {
      m_port = ntohs(((struct sockaddr_in *)&addr)->sin_port);
    }
    XPP_ASSERT(m_port > 0, "EvilServer: getsockname failed");
  }

  /** @brief The kernel-assigned port. */
  uint16_t port() const noexcept {
    return m_port;
  }

  /** @brief Close the listener. Idempotent. */
  void close() {
    if (m_listener) {
      xTcpListenerDestroy(m_listener);
      m_listener = nullptr;
    }
  }

private:
  EvilSpec     m_spec;
  xTcpListener m_listener = nullptr;
  uint16_t     m_port     = 0;

  /* ── Connection handling: read request, send truncated response ── */

  static void on_accept(xTcpListener /*listener*/, xTcpConn conn, const struct sockaddr * /*addr*/,
                        socklen_t /*addrlen*/, void        *arg) {
    auto *self = static_cast<EvilServer *>(arg);

    // Switch the connection's socket to read mode; accumulate until the
    // full header block arrives, then send the truncated response.
    xSocket sock = xTcpConnSocket(conn);
    XPP_ASSERT(sock != nullptr, "EvilServer: xTcpConnSocket failed");

    auto *st = new ConnState;
    st->self = self;
    st->conn = conn;
    st->sock = sock;
    xSocketSetCallback(sock, on_event, st);
    xSocketSetMask(sock, xEvent_Read);
  }

  struct ConnState {
    EvilServer *self;
    xTcpConn    conn;
    xSocket     sock;
    char        buf[8192];
    size_t      used         = 0;
    bool        headers_done = false;
    bool        responded    = false;
    // Send path (after headers arrive):
    char  *send_buf = nullptr;
    size_t send_len = 0;
    size_t send_off = 0;
  };

  static void on_event(xSocket /*sock*/, xEventMask mask, void *arg) {
    auto *st = static_cast<ConnState *>(arg);
    if (mask & xEvent_Read) on_readable(st);
    if (mask & xEvent_Write) pump(st);
  }

  static void on_readable(ConnState *st) {
    ssize_t n = xTcpConnRecv(st->conn, st->buf + st->used, sizeof(st->buf) - st->used);
    if (n <= 0) {
      destroy_conn(st);
      return;
    }
    st->used += static_cast<size_t>(n);

    // Check for end of headers (\r\n\r\n)
    if (!st->headers_done) {
      for (size_t i = 0; i + 4 <= st->used; ++i) {
        if (st->buf[i] == '\r' && st->buf[i + 1] == '\n' && st->buf[i + 2] == '\r' &&
            st->buf[i + 3] == '\n') {
          st->headers_done = true;
          break;
        }
      }
    }

    if (st->headers_done && !st->responded) {
      st->responded = true;
      build_and_send(st);
    }
  }

  static void build_and_send(ConnState *st) {
    const EvilSpec &spec = st->self->m_spec;

    // Build the lying response: full Content-Length, partial body.
    size_t declared = spec.body.size();
    size_t actual   = spec.send_only < declared ? spec.send_only : declared;

    // Status line + headers + body prefix (one buffer for a single send)
    char   header[256];
    size_t hlen = static_cast<size_t>(snprintf(header, sizeof(header),
                                               "HTTP/1.1 200 OK\r\n"
                                               "Content-Length: %zu\r\n"
                                               "Connection: close\r\n"
                                               "\r\n",
                                               declared));

    // Combine header + partial body into the send buffer
    size_t total = hlen + actual;
    auto  *out   = new char[total];
    memcpy(out, header, hlen);
    if (actual > 0) memcpy(out + hlen, spec.body.data(), actual);

    st->send_buf = out;
    st->send_len = total;
    st->send_off = 0;

    xSocketSetMask(st->sock, xEvent_Write);
    pump(st);
  }

  static void pump(ConnState *st) {
    if (!st->send_buf) return;
    ssize_t n = xTcpConnSend(st->conn, st->send_buf + st->send_off, st->send_len - st->send_off);
    if (n <= 0) {
      destroy_conn(st);
      return;
    }
    st->send_off += static_cast<size_t>(n);
    if (st->send_off >= st->send_len) {
      // All bytes sent — hard-close immediately (the "evil" part).
      destroy_conn(st);
    }
  }

  static void destroy_conn(ConnState *st) {
    delete[] st->send_buf;
    st->send_buf = nullptr;
    xTcpConnClose(st->conn); // also destroys the xSocket
    delete st;
  }
};

} // namespace test
} // namespace http
} // namespace xpp

#endif // XPP_HTTP_TEST_EVIL_SERVER_H
