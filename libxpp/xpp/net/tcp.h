/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp.h - xpp::net::TcpConn and TcpListener: Promise-based async TCP.
 *
 * TcpConn wraps xTcpConn + AsyncFd. recv/send use io::read/write
 * (fast-path syscall + EAGAIN readiness wait). connect() and accept()
 * use adapt() — callback → PromiseResolver.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_NET_TCP_H
#define XPP_NET_TCP_H

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <xpp/io/async_fd.h>
#include <xpp/io/error.h>
#include <xpp/net/addr.h>
#include <xpp/net/dns.h>
#include <xpp/net/tls.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/rc.h>
#include <xpp/result.h>

#include <x/base/error.h>
#include <x/base/event.h>
#include <x/net/dns.h>
#include <x/net/tcp.h>

namespace xpp {
namespace net {

/* ── TcpConn ───────────────────────────────────────────────────────── */

namespace _ {
class TcpConnectAdapter;
}

/**
 * @brief RAII async TCP connection.
 *
 * Wraps xTcpConn. I/O via AsyncFd (non-blocking, event-driven).
 * Destructor closes the connection.
 */
class TcpConn {
public:
  /** @brief Async connect (no TLS). Resolves to TcpConn (or fd == -1 on error). */
  static Promise<TcpConn> connect(const char *host, uint16_t port,
                                  Option<const TlsContext &> tls = none);

  /** @brief Async connect by SocketAddr (skips DNS). */
  static Promise<TcpConn> connect(SocketAddr addr, Option<const TlsContext &> tls = none);

  TcpConn() = default;
  TcpConn(TcpConn &&o) noexcept : m_conn(o.m_conn), m_async(std::move(o.m_async)) {
    o.m_conn = nullptr;
  }
  TcpConn &operator=(TcpConn &&o) noexcept {
    if (this != &o) {
      close();
      m_conn   = o.m_conn;
      m_async  = std::move(o.m_async);
      o.m_conn = nullptr;
    }
    return *this;
  }
  TcpConn(const TcpConn &)            = delete;
  TcpConn &operator=(const TcpConn &) = delete;

  ~TcpConn() {
    close();
  }

  /** @brief Async read via AsyncFd. */
  Promise<ssize_t> recv(void *buf, size_t len) {
    if (!m_async.is_closed()) return io::read(m_async, buf, len);
    return xpp::resolve(static_cast<ssize_t>(-1));
  }

  /** @brief Async write via AsyncFd. */
  Promise<ssize_t> send(const void *buf, size_t len) {
    if (!m_async.is_closed()) return io::write(m_async, buf, len);
    return xpp::resolve(static_cast<ssize_t>(-1));
  }

  /** @brief Close and release resources. */
  void close() {
    m_async.close(); // deregister from event loop, wake waiters
    if (m_conn) {
      xTcpConnClose(m_conn);
      m_conn = nullptr;
    }
  }

  int fd() const {
    return m_async.fd();
  }
  bool is_open() const {
    return m_conn != nullptr;
  }

  /** @brief Get peer address. */
  Option<SocketAddr> peer_addr() const {
    if (!m_conn) return none;
    return _::peername(m_async.fd());
  }

  /** @brief Get local address. */
  Option<SocketAddr> local_addr() const {
    if (!m_conn) return none;
    return _::sockname(m_async.fd());
  }

private:
  friend class _::TcpConnectAdapter;
  friend class TcpListener;

  xTcpConn    m_conn = nullptr;
  io::AsyncFd m_async{-1};

  /** @brief Low-level connect using a raw xTcpConnectConf (escape hatch). */
  static Promise<TcpConn> connect_with_conf(const char *host, uint16_t port,
                                            const xTcpConnectConf *conf);

  explicit TcpConn(xTcpConn conn) : m_conn(conn) {
    int fd = fd_from_conn(conn);
    if (fd >= 0) {
      io::_::set_nonblocking(fd);
      m_async = io::AsyncFd(fd);
    }
  }

  static int fd_from_conn(xTcpConn conn) {
    if (!conn) return -1;
    xSocket sock = xTcpConnSocket(conn);
    return sock ? xSocketFd(sock) : -1;
  }
};

/* ── TcpListener ──────────────────────────────────────────────────── */

namespace _ {
class TcpAcceptAdapter;
}

/**
 * @brief RAII async TCP listener.
 *
 * Wraps xTcpListener. accept() returns Promise<TcpConn> via adapt().
 *
 * The underlying state (listener handle + pending resolver) lives in an
 * Rc<Impl> rather than directly in TcpListener because libx's
 * xTcpListenerCreate stores a raw `void* arg` pointer for the accept
 * callback. That pointer must remain stable for the listener's lifetime,
 * but TcpListener itself is movable (returned from bind()). Rc<Impl>
 * heap-allocates the state at a fixed address; moving TcpListener only
 * moves the Rc handle (a pointer), not the Impl. Single-threaded, so
 * Rc (not Arc) is correct — no atomic overhead.
 */
class TcpListener {
public:
  /**
   * @brief Bind to a SocketAddr.
   *
   * Sync bind wrapped in a Promise (resolves immediately).
   * Returns Err(io::Error) on failure.
   */
  static Promise<io::Result<TcpListener>> bind(SocketAddr addr) {
    auto        impl = Rc<Impl>::make();
    std::string ip   = addr.ip().to_string();
    impl->listener   = xTcpListenerCreate(ip.c_str(), addr.port(), nullptr, on_accept, impl.get());
    if (!impl->listener) {
      return xpp::resolve(
        io::Result<TcpListener>(xpp::err, io::Error::from_kind(io::ErrorKind::Other)));
    }
    return xpp::resolve(io::Result<TcpListener>(xpp::ok, TcpListener(std::move(impl))));
  }

  /**
   * @brief Bind to an address string.
   *
   * Accepts "IP:port" (e.g. "127.0.0.1:9090", "[::1]:9090") — resolved
   * synchronously. Also accepts "hostname:port" (e.g. "localhost:9090") —
   * resolved asynchronously via lookup_host().
   *
   * Returns Err(InvalidInput) on malformed input, Err(HostNotFound)
   * if the hostname can't be resolved, or Err(io::Error) on bind failure.
   */
  static Promise<io::Result<TcpListener>> bind(const char *addr) {
    if (!addr || *addr == '\0') {
      return xpp::resolve(
        io::Result<TcpListener>(xpp::err, io::Error::from_kind(io::ErrorKind::InvalidInput)));
    }

    auto parsed = SocketAddr::parse(addr);
    if (parsed.is_ok()) {
      return bind(parsed.unwrap());
    }

    auto hp_r = parse_host_port(addr);
    if (hp_r.is_err()) {
      return xpp::resolve(io::Result<TcpListener>(xpp::err, hp_r.unwrap_err()));
    }
    auto hp = std::move(hp_r).unwrap();

    return lookup_host(hp.first.c_str()).then([port = hp.second](std::vector<SocketAddr> addrs) {
      if (addrs.empty()) {
        return xpp::resolve(
          io::Result<TcpListener>(xpp::err, io::Error::from_kind(io::ErrorKind::HostNotFound)));
      }
      return bind(addrs[0]);
    });
  }

  TcpListener()                                   = default;
  TcpListener(TcpListener &&) noexcept            = default;
  TcpListener &operator=(TcpListener &&) noexcept = default;
  TcpListener(const TcpListener &)                = delete;
  TcpListener &operator=(const TcpListener &)     = delete;

  ~TcpListener() = default;

  /** @brief Accept next connection. Resolves when a peer connects. */
  Promise<TcpConn> accept() {
    Impl *impl = m_impl.as_deref();
    if (!impl || !impl->listener) return xpp::resolve(TcpConn());
    return xpp::adapt<TcpConn, _::TcpAcceptAdapter>(impl);
  }

  bool is_open() const {
    const Impl *impl = m_impl.as_deref();
    return impl && impl->listener;
  }

private:
  friend class _::TcpAcceptAdapter;

  // State lives on the heap so the libx callback's void* arg stays stable
  // across TcpListener moves. ~Impl destroys the listener handle.
  struct Impl {
    xTcpListener             listener = nullptr;
    PromiseResolver<TcpConn> pending;

    ~Impl() {
      if (listener) xTcpListenerDestroy(listener);
    }
  };
  Option<Rc<Impl>> m_impl;

  explicit TcpListener(Rc<Impl> impl) : m_impl(some(std::move(impl))) {}

  static void on_accept(xTcpListener, xTcpConn conn, const struct sockaddr *, socklen_t,
                        void *arg) {
    auto *impl = static_cast<Impl *>(arg);
    if (impl->pending.is_pending()) {
      auto r = std::move(impl->pending);
      r.resolve(TcpConn(conn));
    } else {
      // No one waiting — close the connection
      xTcpConnClose(conn);
    }
  }
};

/* ── DNS ──────────────────────────────────────────────────────────── */

// lookup_host() and LookupHostAdapter are defined in <xpp/net/dns.h>.

/* ── Internal adapters ────────────────────────────────────────────── */
namespace _ {

/// TCP connect adapter
class TcpConnectAdapter {
private:
  PromiseResolver<TcpConn> m_resolver;
  xTcpConnectConf          m_conf{};
  std::string              m_host;

public:
  TcpConnectAdapter(PromiseResolver<TcpConn> r, const char *host, uint16_t port,
                    const xTcpConnectConf *conf)
      : m_resolver(std::move(r)), m_host(host) {
    if (conf) m_conf = *conf;
    xTcpConnect(m_host.c_str(), port, &m_conf, on_connect, this);
  }

  ~TcpConnectAdapter() {
    // xTcpConnect doesn't have a cancel API — the callback will fire
    // eventually. PromiseResolver uses ArcWeak, so if the Promise is
    // destroyed, resolve() is a no-op.
  }

  TcpConnectAdapter(const TcpConnectAdapter &)            = delete;
  TcpConnectAdapter &operator=(const TcpConnectAdapter &) = delete;
  TcpConnectAdapter(TcpConnectAdapter &&)                 = delete;
  TcpConnectAdapter &operator=(TcpConnectAdapter &&)      = delete;

private:
  static void on_connect(xTcpConn conn, xErrno err, void *arg) {
    auto *self = static_cast<TcpConnectAdapter *>(arg);
    if (conn && err == xErrno_Ok) {
      self->m_resolver.resolve(TcpConn(conn));
    } else {
      self->m_resolver.resolve(TcpConn()); // empty = error
    }
    delete self; // self-delete — adapter lifetime ends here
  }
};

/// TCP accept adapter
class TcpAcceptAdapter {
private:
  TcpListener::Impl *m_impl;

public:
  TcpAcceptAdapter(PromiseResolver<TcpConn> r, TcpListener::Impl *impl) : m_impl(impl) {
    m_impl->pending = std::move(r);
  }

  ~TcpAcceptAdapter()                                   = default;
  TcpAcceptAdapter(const TcpAcceptAdapter &)            = delete;
  TcpAcceptAdapter &operator=(const TcpAcceptAdapter &) = delete;
  TcpAcceptAdapter(TcpAcceptAdapter &&)                 = delete;
  TcpAcceptAdapter &operator=(TcpAcceptAdapter &&)      = delete;
};

/// DNS resolve adapter — defined in <xpp/net/dns.h>

} // namespace _

/* ── Implementations ──────────────────────────────────────────────── */

inline Promise<TcpConn> TcpConn::connect_with_conf(const char *host, uint16_t port,
                                                   const xTcpConnectConf *conf) {
  // TcpConnectAdapter self-deletes in its callback — xTcpConnect has no
  // cancel API, so the adapter must outlive the Promise. If the Promise is
  // dropped first, the callback still fires and resolves to a no-op via
  // ArcWeak. Connect timeout (default 10s) bounds the leak.
  auto pr = xpp::async<TcpConn>();
  new _::TcpConnectAdapter(std::move(pr.second), host, port, conf);
  return std::move(pr.first);
}

inline Promise<TcpConn> TcpConn::connect(const char *host, uint16_t port,
                                         Option<const TlsContext &> tls) {
  xTcpConnectConf conf{};
  if (tls.is_some() && tls.unwrap().is_valid()) {
    conf.tls_ctx = tls.unwrap().raw();
  }
  return connect_with_conf(host, port, &conf);
}

inline Promise<TcpConn> TcpConn::connect(SocketAddr addr, Option<const TlsContext &> tls) {
  // Pass the IP string directly — xTcpConnect parses literal IPs without DNS.
  std::string ip = addr.ip().to_string();
  return connect(ip.c_str(), addr.port(), tls);
}

} // namespace net
} // namespace xpp

#endif // XPP_NET_TCP_H
