/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp.h - xpp::net::TcpStream and TcpListener: Promise-based async TCP.
 *
 * TcpStream wraps xTcpConn + AsyncFd. recv/send use io::read/write
 * (fast-path syscall + EAGAIN readiness wait). accept() uses
 * adapt(); connect() uses async() + self-delete (xTcpConnect has no
 * cancel API, adapter must outlive the Promise).
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_NET_TCP_H
#define XPP_NET_TCP_H

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <xpp/handle.h>
#include <xpp/io/async_fd.h>
#include <xpp/io/error.h>
#include <xpp/net/addr.h>
#include <xpp/net/dns.h>
#include <xpp/net/tls.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/promise_utils.h>
#include <xpp/rc.h>
#include <xpp/result.h>
#include <xpp/shared.h>

#include <x/base/error.h>
#include <x/base/event.h>
#include <x/net/dns.h>
#include <x/net/tcp.h>

namespace xpp {
namespace net {

/* ── TcpStream ───────────────────────────────────────────────────────── */

namespace _ {
class TcpConnectAdapter;
}

/**
 * @brief RAII async TCP connection.
 *
 * Wraps xTcpConn. I/O via AsyncFd (non-blocking, event-driven).
 * Destructor closes the connection.
 */
class TcpStream {
public:
  /**
   * @brief Async connect to an address string.
   *
   * Accepts "IP:port" (e.g. "127.0.0.1:9090", "[::1]:9090") — resolved
   * synchronously. Also accepts "hostname:port" (e.g. "example.com:80") —
   * resolved asynchronously via lookup_host().
   *
   * Returns Err(InvalidInput) on malformed input, Err(HostNotFound)
   * if the hostname can't be resolved, or Err(io::Error) on connect failure.
   *
   * Aligned with TcpListener::bind(const char *addr).
   */
  static Promise<io::Result<TcpStream>> connect(const char                *addr,
                                                Option<const TlsContext &> tls = none);

  /** @brief Async connect to a SocketAddr (skips DNS). */
  static Promise<io::Result<TcpStream>> connect(SocketAddr                 addr,
                                                Option<const TlsContext &> tls = none);

  TcpStream() = default;
  TcpStream(TcpStream &&o) noexcept : m_conn(std::move(o.m_conn)), m_async(std::move(o.m_async)) {}
  TcpStream &operator=(TcpStream &&o) noexcept {
    if (this != &o) {
      close();
      m_conn  = std::move(o.m_conn);
      m_async = std::move(o.m_async);
    }
    return *this;
  }
  TcpStream(const TcpStream &)            = delete;
  TcpStream &operator=(const TcpStream &) = delete;

  ~TcpStream() {
    close();
  }

  /** @brief Async read via AsyncFd. */
  Promise<ssize_t> read(void *buf, size_t len) {
    if (!m_async.is_closed()) return io::read(m_async, buf, len);
    return xpp::resolve(static_cast<ssize_t>(-1));
  }

  /** @brief Async write via AsyncFd. */
  Promise<ssize_t> write(const void *buf, size_t len) {
    if (!m_async.is_closed()) return io::write(m_async, buf, len);
    return xpp::resolve(static_cast<ssize_t>(-1));
  }

  /** @brief Synchronous non-blocking read. Returns -1 with errno=EAGAIN if no data. */
  ssize_t try_read(void *buf, size_t len) {
    if (m_async.is_closed()) return -1;
    return ::read(m_async.fd(), buf, len);
  }

  /** @brief Synchronous non-blocking write. Returns -1 with errno=EAGAIN if buffer full. */
  ssize_t try_write(const void *buf, size_t len) {
    if (m_async.is_closed()) return -1;
    return ::write(m_async.fd(), buf, len);
  }

  /** @brief Wait for readability (forward to AsyncFd). */
  Promise<void> readable() {
    return m_async.readable();
  }

  /** @brief Wait for writability (forward to AsyncFd). */
  Promise<void> writable() {
    return m_async.writable();
  }

  /** @brief Peek without consuming. Same fast-path + EAGAIN pattern as read. */
  Promise<ssize_t> peek(void *buf, size_t len) {
    if (m_async.is_closed()) return xpp::resolve(static_cast<ssize_t>(-1));
    int     fd = m_async.fd();
    ssize_t n  = ::recv(fd, buf, len, MSG_PEEK);
    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return xpp::resolve(n);
    return m_async.readable().then([fd, buf, len] { return ::recv(fd, buf, len, MSG_PEEK); });
  }

  /** @brief Get and clear the pending socket error (SO_ERROR). Returns 0 if no error. */
  int take_error() {
    if (m_async.is_closed()) return -1;
    int       err = 0;
    socklen_t len = sizeof(err);
    getsockopt(m_async.fd(), SOL_SOCKET, SO_ERROR, &err, &len);
    return err;
  }

  /** @brief Get TCP_NODELAY. */
  io::Result<bool> nodelay() const {
    if (m_async.is_closed())
      return io::Result<bool>(xpp::err, io::Error::from_kind(io::ErrorKind::Other));
    int       val = 0;
    socklen_t len = sizeof(val);
    if (getsockopt(m_async.fd(), IPPROTO_TCP, TCP_NODELAY, &val, &len) != 0)
      return io::Result<bool>(xpp::err, io::Error::from_errno(errno));
    return io::Result<bool>(xpp::ok, val != 0);
  }

  /** @brief Set TCP_NODELAY. Returns Ok(true) on success. */
  io::Result<bool> set_nodelay(bool enable) {
    if (m_async.is_closed())
      return io::Result<bool>(xpp::err, io::Error::from_kind(io::ErrorKind::Other));
    int val = enable ? 1 : 0;
    if (setsockopt(m_async.fd(), IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) != 0)
      return io::Result<bool>(xpp::err, io::Error::from_errno(errno));
    return io::Result<bool>(xpp::ok, true);
  }

  /** @brief Get IP_TTL. */
  io::Result<uint32_t> ttl() const {
    if (m_async.is_closed())
      return io::Result<uint32_t>(xpp::err, io::Error::from_kind(io::ErrorKind::Other));
    uint32_t  val = 0;
    socklen_t len = sizeof(val);
    if (getsockopt(m_async.fd(), IPPROTO_IP, IP_TTL, &val, &len) != 0)
      return io::Result<uint32_t>(xpp::err, io::Error::from_errno(errno));
    return io::Result<uint32_t>(xpp::ok, val);
  }

  /** @brief Set IP_TTL. Returns Ok(true) on success. */
  io::Result<bool> set_ttl(uint32_t ttl) {
    if (m_async.is_closed())
      return io::Result<bool>(xpp::err, io::Error::from_kind(io::ErrorKind::Other));
    if (setsockopt(m_async.fd(), IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) != 0)
      return io::Result<bool>(xpp::err, io::Error::from_errno(errno));
    return io::Result<bool>(xpp::ok, true);
  }

  /** @brief Get SO_LINGER seconds, or -1 if disabled. */
  io::Result<int32_t> linger() const {
    if (m_async.is_closed())
      return io::Result<int32_t>(xpp::err, io::Error::from_kind(io::ErrorKind::Other));
    struct linger lng = {};
    socklen_t     len = sizeof(lng);
    if (getsockopt(m_async.fd(), SOL_SOCKET, SO_LINGER, &lng, &len) != 0)
      return io::Result<int32_t>(xpp::err, io::Error::from_errno(errno));
    return io::Result<int32_t>(xpp::ok, lng.l_onoff ? lng.l_linger : -1);
  }

  /** @brief Set SO_LINGER seconds. Pass -1 to disable. */
  io::Result<bool> set_linger(int32_t seconds) {
    if (m_async.is_closed())
      return io::Result<bool>(xpp::err, io::Error::from_kind(io::ErrorKind::Other));
    struct linger lng;
    lng.l_onoff  = seconds >= 0 ? 1 : 0;
    lng.l_linger = seconds >= 0 ? seconds : 0;
    if (setsockopt(m_async.fd(), SOL_SOCKET, SO_LINGER, &lng, sizeof(lng)) != 0)
      return io::Result<bool>(xpp::err, io::Error::from_errno(errno));
    return io::Result<bool>(xpp::ok, true);
  }

  /** @brief Close and release resources. */
  void close() {
    m_async.close(); // deregister from event loop, wake waiters
    m_conn = {};
  }

  int fd() const {
    return m_async.fd();
  }
  bool is_open() const {
    return m_conn.get() != nullptr;
  }

  /** @brief Get peer address. */
  Option<SocketAddr> peer_addr() const {
    if (!m_conn.get()) return none;
    return peername(m_async.fd());
  }

  /** @brief Get local address. */
  Option<SocketAddr> local_addr() const {
    if (!m_conn.get()) return none;
    return sockname(m_async.fd());
  }

private:
  friend class _::TcpConnectAdapter;
  friend class TcpListener;

  struct Destroy {
    void deallocate(void *p, Layout) const noexcept {
      if (p) xTcpConnClose(static_cast<xTcpConn>(p));
    }
  };
  OwnedHandle<Destroy> m_conn;
  io::AsyncFd          m_async{-1};

  /** @brief Low-level connect using a raw xTcpConnectConf (escape hatch). */
  static Promise<io::Result<TcpStream>> connect_with_conf(const char *host, uint16_t port,
                                                          const xTcpConnectConf *conf);

  explicit TcpStream(xTcpConn conn) : m_conn(conn) {
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
 * Wraps xTcpListener. accept() returns Promise<TcpStream> via adapt().
 *
 * The underlying state (listener handle + pending resolver) lives in an
 * Shared<Impl> rather than directly in TcpListener because libx's
 * xTcpListenerCreate stores a raw `void* arg` pointer for the accept
 * callback. That pointer must remain stable for the listener's lifetime,
 * but TcpListener itself is movable (returned from bind()). Shared<Impl>
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
    auto        impl = Shared<Impl>::make();
    std::string ip   = addr.ip().to_string();
    impl->listener   = OwnedHandle<Impl::Destroy>(
      xTcpListenerCreate(ip.c_str(), addr.port(), nullptr, on_accept, impl.get()));
    if (!impl->listener.get()) {
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

  /** @brief Accept next connection. Resolves to {TcpStream, SocketAddr} when a peer connects. */
  Promise<std::pair<TcpStream, SocketAddr>> accept() {
    Impl *impl = m_impl.as_deref();
    if (!impl || !impl->listener.get())
      return xpp::resolve(std::make_pair(TcpStream(), SocketAddr::unspecified()));
    return xpp::adapt<std::pair<TcpStream, SocketAddr>, _::TcpAcceptAdapter>(impl);
  }

  bool is_open() const {
    const Impl *impl = m_impl.as_deref();
    return impl && impl->listener.get();
  }

  /** @brief Get the address this listener is bound to. */
  Option<SocketAddr> local_addr() const {
    const Impl *impl = m_impl.as_deref();
    if (!impl || !impl->listener.get()) return none;
    xSocket sock = xTcpListenerSocket(static_cast<xTcpListener>(impl->listener.get()));
    if (!sock) return none;
    return sockname(xSocketFd(sock));
  }

private:
  friend class _::TcpAcceptAdapter;

  // State lives on the heap so the libx callback's void* arg stays stable
  // across TcpListener moves. ~Impl destroys the listener handle.
  struct Impl {
    struct Destroy {
      void deallocate(void *p, Layout) const noexcept {
        if (p) xTcpListenerDestroy(static_cast<xTcpListener>(p));
      }
    };
    OwnedHandle<Destroy>                              listener;
    PromiseResolver<std::pair<TcpStream, SocketAddr>> pending;
  };
  Option<Shared<Impl>> m_impl;

  explicit TcpListener(Shared<Impl> impl) : m_impl(some(std::move(impl))) {}

  static void on_accept(xTcpListener, xTcpConn conn, const struct sockaddr *sa, socklen_t slen,
                        void *arg) {
    auto *impl = static_cast<Impl *>(arg);
    if (impl->pending.is_pending()) {
      auto r    = std::move(impl->pending);
      auto addr = SocketAddr::from_sockaddr(sa, slen);
      r.resolve(std::make_pair(TcpStream(conn), addr.is_some() ? std::move(addr).unwrap()
                                                               : SocketAddr::unspecified()));
    } else {
      // No one waiting — close the connection
      if (conn) xTcpConnClose(conn);
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
  PromiseResolver<io::Result<TcpStream>> m_resolver;
  xTcpConnectConf                        m_conf{};
  std::string                            m_host;

public:
  TcpConnectAdapter(PromiseResolver<io::Result<TcpStream>> r, const char *host, uint16_t port,
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
      self->m_resolver.resolve(io::Result<TcpStream>(xpp::ok, TcpStream(conn)));
    } else {
      self->m_resolver.resolve(io::Result<TcpStream>(xpp::err, io::Error::from_xerrno(err)));
    }
    // SAFETY: xTcpConnect callback is always async (posted via event loop).
    // If it ever fires synchronously, this self-delete would be UB.
    delete self; // self-delete — adapter lifetime ends here
  }
};

/// TCP accept adapter
class TcpAcceptAdapter {
private:
  TcpListener::Impl *m_impl;

public:
  TcpAcceptAdapter(PromiseResolver<std::pair<TcpStream, SocketAddr>> r, TcpListener::Impl *impl)
      : m_impl(impl) {
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

inline Promise<io::Result<TcpStream>> TcpStream::connect_with_conf(const char *host, uint16_t port,
                                                                   const xTcpConnectConf *conf) {
  // TcpConnectAdapter self-deletes in its callback — xTcpConnect has no
  // cancel API, so the adapter must outlive the Promise. If the Promise is
  // dropped first, the callback still fires and resolves to a no-op via
  // ArcWeak. Connect timeout (default 10s) bounds the leak.
  auto pr = xpp::async<io::Result<TcpStream>>();
  new _::TcpConnectAdapter(std::move(pr.second), host, port, conf);
  return std::move(pr.first);
}

inline Promise<io::Result<TcpStream>> TcpStream::connect(const char                *addr,
                                                         Option<const TlsContext &> tls) {
  if (!addr || *addr == '\0') {
    return xpp::resolve(
      io::Result<TcpStream>(xpp::err, io::Error::from_kind(io::ErrorKind::InvalidInput)));
  }

  // IP literal → connect directly (no DNS)
  auto parsed = SocketAddr::parse(addr);
  if (parsed.is_ok()) {
    return connect(parsed.unwrap(), tls);
  }

  // hostname:port → async DNS → try each resolved address (happy eyeballs)
  auto hp_r = parse_host_port(addr);
  if (hp_r.is_err()) {
    return xpp::resolve(io::Result<TcpStream>(xpp::err, hp_r.unwrap_err()));
  }
  auto hp = std::move(hp_r).unwrap();

  return lookup_host(hp.first.c_str()).then([port = hp.second, tls](std::vector<SocketAddr> addrs) {
    if (addrs.empty()) {
      return xpp::resolve(
        io::Result<TcpStream>(xpp::err, io::Error::from_kind(io::ErrorKind::HostNotFound)));
    }

    // Build conf once (TlsContext* inside survives as long as TlsContext does).
    auto conf = Shared<xTcpConnectConf>::make();
    if (tls.is_some() && tls.unwrap().is_valid()) {
      conf->tls_ctx = tls.unwrap().raw();
    }

    // Tail-recursive happy eyeballs: try each resolved address
    return xpp::try_next(std::move(addrs), [conf](const SocketAddr &a) {
      return TcpStream::connect_with_conf(a.ip().to_string().c_str(), a.port(), conf.get());
    })();
  });
}

inline Promise<io::Result<TcpStream>> TcpStream::connect(SocketAddr                 addr,
                                                         Option<const TlsContext &> tls) {
  std::string     ip = addr.ip().to_string();
  xTcpConnectConf conf{};
  if (tls.is_some() && tls.unwrap().is_valid()) {
    conf.tls_ctx = tls.unwrap().raw();
  }
  return connect_with_conf(ip.c_str(), addr.port(), &conf);
}

} // namespace net
} // namespace xpp

#endif // XPP_NET_TCP_H
