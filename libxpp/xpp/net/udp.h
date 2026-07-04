/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * udp.h - xpp::net::UdpSocket: Promise-based async UDP.
 *
 * libx has no UDP API, so UdpSocket is built directly on ::socket(),
 * ::bind(), and AsyncFd. recv_from/send_to use the fast-path syscall
 * + EAGAIN readiness wait pattern (same as io::read/io::write).
 *
 * recv_from returns Promise<pair<ssize_t, SocketAddr>> — bytes read
 * + peer address. C++11 callers use std::tie; C++17 callers may use
 * structured bindings.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_NET_UDP_H
#define XPP_NET_UDP_H

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
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
#include <xpp/promise.h>
#include <xpp/result.h>

#include <x/base/error.h>

namespace xpp {
namespace net {

/**
 * @brief RAII async UDP socket.
 *
 * Wraps a non-blocking UDP fd registered with the event loop via AsyncFd.
 * recv_from / send_to use the fast-path syscall + EAGAIN readiness wait
 * pattern. Destructor closes the fd.
 *
 * Buffer lifetime: `buf` pointers passed to recv_from / send_to must
 * remain valid until the returned Promise resolves.
 *
 * Move-only. Moved-from state is a tombstone (AsyncFd::is_closed()).
 */
class UdpSocket {
public:
  UdpSocket() = default;

  UdpSocket(UdpSocket &&o) noexcept : m_async(std::move(o.m_async)) {}
  UdpSocket &operator=(UdpSocket &&o) noexcept {
    if (this != &o) {
      close();
      m_async = std::move(o.m_async);
    }
    return *this;
  }
  UdpSocket(const UdpSocket &)            = delete;
  UdpSocket &operator=(const UdpSocket &) = delete;

  ~UdpSocket() {
    close();
  }

  /**
   * @brief Bind to a SocketAddr.
   *
   * Sync bind wrapped in a Promise (resolves immediately).
   * Returns Err(io::Error) on socket()/bind() failure.
   */
  static Promise<io::Result<UdpSocket>> bind(SocketAddr addr) {
    struct sockaddr_storage ss;
    socklen_t               slen;
    addr.to_sockaddr(&ss, &slen);

    int fd = ::socket(ss.ss_family, SOCK_DGRAM, 0);
    if (fd < 0) {
      return xpp::resolve(io::Result<UdpSocket>(xpp::err, io::Error::from_errno(errno)));
    }

    io::_::set_nonblocking(fd);

    if (::bind(fd, reinterpret_cast<struct sockaddr *>(&ss), slen) != 0) {
      int err = errno;
      ::close(fd);
      return xpp::resolve(io::Result<UdpSocket>(xpp::err, io::Error::from_errno(err)));
    }

    return xpp::resolve(io::Result<UdpSocket>(xpp::ok, UdpSocket(fd)));
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
  static Promise<io::Result<UdpSocket>> bind(const char *addr) {
    if (!addr || *addr == '\0') {
      return xpp::resolve(
        io::Result<UdpSocket>(xpp::err, io::Error::from_kind(io::ErrorKind::InvalidInput)));
    }

    auto parsed = SocketAddr::parse(addr);
    if (parsed.is_ok()) {
      return bind(parsed.unwrap());
    }

    auto hp_r = parse_host_port(addr);
    if (hp_r.is_err()) {
      return xpp::resolve(io::Result<UdpSocket>(xpp::err, hp_r.unwrap_err()));
    }
    auto hp = std::move(hp_r).unwrap();

    return lookup_host(hp.first.c_str()).then([port = hp.second](std::vector<SocketAddr> addrs) {
      if (addrs.empty()) {
        return xpp::resolve(
          io::Result<UdpSocket>(xpp::err, io::Error::from_kind(io::ErrorKind::HostNotFound)));
      }
      return bind(addrs[0]);
    });
  }

  Promise<std::pair<ssize_t, SocketAddr>> recv_from(void *buf, size_t len) {
    if (m_async.is_closed()) {
      return xpp::resolve(std::make_pair(static_cast<ssize_t>(-1), SocketAddr::unspecified()));
    }
    return recv_from_step(buf, len);
  }

  Promise<ssize_t> send_to(const void *buf, size_t len, SocketAddr target) {
    if (m_async.is_closed()) return xpp::resolve(static_cast<ssize_t>(-1));

    struct sockaddr_storage ss;
    socklen_t               slen;
    target.to_sockaddr(&ss, &slen);

    int     fd = m_async.fd();
    ssize_t n  = ::sendto(fd, buf, len, 0, reinterpret_cast<struct sockaddr *>(&ss), slen);
    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
      return xpp::resolve(n);
    }
    return m_async.writable().then([this, buf, len, target] { return send_to(buf, len, target); });
  }

  /** @brief Close the socket and deregister from the event loop. */
  void close() {
    if (!m_async.is_closed()) {
      int fd = m_async.fd();
      m_async.close();
      ::close(fd);
    }
  }

  int fd() const {
    return m_async.fd();
  }
  bool is_open() const {
    return !m_async.is_closed();
  }

  /** @brief Bound local address. None on error or after close. */
  Option<SocketAddr> local_addr() const {
    if (m_async.is_closed()) return none;
    return _::sockname(m_async.fd());
  }

private:
  io::AsyncFd m_async{-1};

  explicit UdpSocket(int fd) : m_async(fd) {}

  Promise<std::pair<ssize_t, SocketAddr>> recv_from_step(void *buf, size_t len) {
    int                     fd = m_async.fd();
    struct sockaddr_storage ss;
    socklen_t               slen = sizeof(ss);
    ssize_t n = ::recvfrom(fd, buf, len, 0, reinterpret_cast<struct sockaddr *>(&ss), &slen);
    if (n >= 0) {
      auto addr = SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&ss), slen);
      return xpp::resolve(
        std::make_pair(n, addr.is_some() ? std::move(addr).unwrap() : SocketAddr::unspecified()));
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return xpp::resolve(std::make_pair(static_cast<ssize_t>(-1), SocketAddr::unspecified()));
    }
    return m_async.readable().then([this, buf, len] { return recv_from_step(buf, len); });
  }
};

} // namespace net
} // namespace xpp

#endif // XPP_NET_UDP_H
