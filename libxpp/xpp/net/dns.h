/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns.h - xpp::net::lookup_host: async DNS resolution.
 *
 * Uses adapt() with LookupHostAdapter. The adapter calls xDnsResolve
 * in its constructor and xDnsCancel in its destructor, so dropping the
 * Promise cancels the query safely.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_NET_DNS_H
#define XPP_NET_DNS_H

#include <sys/socket.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <xpp/io/error.h>
#include <xpp/net/addr.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/result.h>

#include <x/net/dns.h>

namespace xpp {
namespace net {

/* ── Socket utilities ─────────────────────────────────────────────── */

namespace _ {

/** @brief Get the local address of a socket fd. Returns None on error. */
inline Option<SocketAddr> sockname(int fd) noexcept {
  struct sockaddr_storage ss;
  socklen_t               len = sizeof(ss);
  if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&ss), &len) != 0) return none;
  return SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&ss), len);
}

/** @brief Get the peer address of a socket fd. Returns None on error. */
inline Option<SocketAddr> peername(int fd) noexcept {
  struct sockaddr_storage ss;
  socklen_t               len = sizeof(ss);
  if (getpeername(fd, reinterpret_cast<struct sockaddr *>(&ss), &len) != 0) return none;
  return SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&ss), len);
}

} // namespace _

/* ── HostPort ─────────────────────────────────────────────────────── */

using HostPort = std::pair<std::string, uint16_t>;

/**
 * @brief Parse a "hostname:port" string.
 *
 * Callers should try SocketAddr::parse() first (for literal IPs).
 * If that fails, this splits on the last ':' and validates the port.
 *
 * Returns Err(InvalidInput) on bad format or port range.
 */
inline io::Result<HostPort> parse_host_port(const char *addr) {
  if (!addr || *addr == '\0') {
    return io::Result<HostPort>(xpp::err, io::Error::from_kind(io::ErrorKind::InvalidInput));
  }
  const char *colon = std::strrchr(addr, ':');
  if (!colon) {
    return io::Result<HostPort>(xpp::err, io::Error::from_kind(io::ErrorKind::InvalidInput));
  }
  std::string host(addr, static_cast<size_t>(colon - addr));
  const char *port_str = colon + 1;
  char       *end      = nullptr;
  long        port     = std::strtol(port_str, &end, 10);
  if (*end != '\0' || port < 0 || port > 65535) {
    return io::Result<HostPort>(xpp::err, io::Error::from_kind(io::ErrorKind::InvalidInput));
  }
  return io::Result<HostPort>(xpp::ok,
                              std::make_pair(std::move(host), static_cast<uint16_t>(port)));
}

/* ── Forward declaration ──────────────────────────────────────────── */

namespace _ {
class LookupHostAdapter;
}

/**
 * @brief Async DNS resolution.
 *
 * @param hostname Hostname to resolve.
 * @return Promise resolving to a vector of SocketAddr (empty on error).
 */
Promise<std::vector<SocketAddr>> lookup_host(const char *hostname);

/* ── Internal adapter ─────────────────────────────────────────────── */

namespace _ {

/// DNS resolve adapter
class LookupHostAdapter {
private:
  PromiseResolver<std::vector<SocketAddr>> m_resolver;
  std::string                              m_host;
  xDnsQuery                                m_query = nullptr;

public:
  LookupHostAdapter(PromiseResolver<std::vector<SocketAddr>> r, const char *hostname)
      : m_resolver(std::move(r)), m_host(hostname) {
    m_query = xDnsResolve(m_host.c_str(), nullptr, nullptr, on_resolve, this);
  }

  ~LookupHostAdapter() {
    // xDnsCancel guarantees the callback will not fire after this returns,
    // so it's safe to destroy the adapter (and m_host) after calling it.
    if (m_query) xDnsCancel(m_query);
  }

  LookupHostAdapter(const LookupHostAdapter &)            = delete;
  LookupHostAdapter &operator=(const LookupHostAdapter &) = delete;
  LookupHostAdapter(LookupHostAdapter &&)                 = delete;
  LookupHostAdapter &operator=(LookupHostAdapter &&)      = delete;

private:
  static void on_resolve(xDnsResult *result, void *arg) {
    auto *self    = static_cast<LookupHostAdapter *>(arg);
    self->m_query = nullptr; // query is done, no need to cancel in dtor

    std::vector<SocketAddr> addrs;
    if (result && result->error == xErrno_Ok) {
      for (xDnsAddr *a = result->addrs; a; a = a->next) {
        auto sa =
          SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&a->addr), a->addrlen);
        if (sa.is_some()) addrs.push_back(std::move(sa).unwrap());
      }
    }
    if (result) xDnsResultFree(result);

    self->m_resolver.resolve(std::move(addrs));
    // No self-delete — AdapterPromiseNode owns this adapter.
  }
};

} // namespace _

/* ── Implementation ───────────────────────────────────────────────── */

inline Promise<std::vector<SocketAddr>> lookup_host(const char *hostname) {
  return xpp::adapt<std::vector<SocketAddr>, _::LookupHostAdapter>(hostname);
}

} // namespace net
} // namespace xpp

#endif // XPP_NET_DNS_H
