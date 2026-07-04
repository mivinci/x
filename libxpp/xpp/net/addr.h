/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * addr.h - IPv4/IPv6 address and socket address types.
 *
 * Mirrors Rust's std::net::{IpAddr, Ipv4Addr, Ipv6Addr,
 * SocketAddr, SocketAddrV4, SocketAddrV6}. All trivially copyable
 * types store bytes in network byte order to match struct in_addr /
 * struct in6_addr layout.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_NET_ADDR_H
#define XPP_NET_ADDR_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/span.h>
#include <xpp/variant.h>

namespace xpp {
namespace net {

/* ── AddrParseError ────────────────────────────────────────────────── */

enum class AddrParseError : uint8_t {
  InvalidIpv4,
  InvalidIpv6,
  InvalidPort,
  InvalidFormat,
};

inline const char *addr_error_message(AddrParseError e) noexcept {
  switch (e) {
  case AddrParseError::InvalidIpv4:
    return "invalid IPv4 address";
  case AddrParseError::InvalidIpv6:
    return "invalid IPv6 address";
  case AddrParseError::InvalidPort:
    return "invalid port number";
  case AddrParseError::InvalidFormat:
    return "invalid address format";
  default:
    return "unknown address error";
  }
}

/* ── Forward declarations ──────────────────────────────────────────── */

class Ipv6Addr;
class IpAddr;
class SocketAddrV4;
class SocketAddrV6;
class SocketAddr;

/* ── Ipv4Addr ──────────────────────────────────────────────────────── */

class Ipv4Addr {
public:
  static constexpr Ipv4Addr from(uint8_t a, uint8_t b, uint8_t c, uint8_t d) noexcept {
    Ipv4Addr r;
    r.m_addr = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
               (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
    return r;
  }

  static constexpr Ipv4Addr localhost() noexcept {
    return from(127, 0, 0, 1);
  }
  static constexpr Ipv4Addr unspecified() noexcept {
    return from(0, 0, 0, 0);
  }
  static constexpr Ipv4Addr broadcast() noexcept {
    return from(255, 255, 255, 255);
  }

  static constexpr Ipv4Addr from_octets(const uint8_t o[4]) noexcept {
    return from(o[0], o[1], o[2], o[3]);
  }

  static inline Result<Ipv4Addr, AddrParseError> parse(Span<const char> s) {
    if (s.is_empty()) return Result<Ipv4Addr, AddrParseError>(err, AddrParseError::InvalidIpv4);
    char   buf[16];
    size_t len = s.size() < 15 ? s.size() : 15;
    memcpy(buf, s.data(), len);
    buf[len] = '\0';
    struct in_addr addr;
    if (inet_pton(AF_INET, buf, &addr) != 1)
      return Result<Ipv4Addr, AddrParseError>(err, AddrParseError::InvalidIpv4);
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&addr);
    return Result<Ipv4Addr, AddrParseError>(ok, Ipv4Addr::from(p[0], p[1], p[2], p[3]));
  }

  static inline Result<Ipv4Addr, AddrParseError> parse(const char *s) {
    return parse(Span<const char>(s, strlen(s)));
  }

  static inline Result<Ipv4Addr, AddrParseError> parse(const std::string &s) {
    return parse(Span<const char>(s.data(), s.size()));
  }

  struct Octets {
    uint8_t data[4];
  };

  constexpr Octets octets() const noexcept {
    return Octets{{static_cast<uint8_t>(m_addr >> 24), static_cast<uint8_t>(m_addr >> 16),
                   static_cast<uint8_t>(m_addr >> 8), static_cast<uint8_t>(m_addr)}};
  }

  constexpr uint8_t operator[](size_t i) const noexcept {
    return static_cast<uint8_t>(m_addr >> (24 - i * 8));
  }

  constexpr bool is_loopback() const noexcept {
    return (m_addr >> 24) == 127;
  }
  constexpr bool is_unspecified() const noexcept {
    return m_addr == 0;
  }
  constexpr bool is_multicast() const noexcept {
    return (m_addr >> 24) >= 224 && (m_addr >> 24) <= 239;
  }
  constexpr bool is_link_local() const noexcept {
    return (m_addr >> 16) == (169 << 8 | 254);
  }
  constexpr bool is_broadcast() const noexcept {
    return m_addr == 0xFFFFFFFF;
  }
  constexpr bool is_private() const noexcept {
    return (m_addr >> 24) == 10 || ((m_addr >> 24) == 172 && ((m_addr >> 16) & 0xF0) == 16) ||
           ((m_addr >> 24) == 192 && ((m_addr >> 16) & 0xFF) == 168);
  }

  inline Ipv6Addr to_ipv6_mapped() const noexcept;
  inline Ipv6Addr to_ipv6_compatible() const noexcept;

  inline std::string to_string() const {
    char           buf[16];
    struct in_addr addr;
    uint8_t       *p = reinterpret_cast<uint8_t *>(&addr);
    p[0]             = static_cast<uint8_t>(m_addr >> 24);
    p[1]             = static_cast<uint8_t>(m_addr >> 16);
    p[2]             = static_cast<uint8_t>(m_addr >> 8);
    p[3]             = static_cast<uint8_t>(m_addr);
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    return std::string(buf);
  }

  constexpr bool operator==(Ipv4Addr o) const noexcept {
    return m_addr == o.m_addr;
  }
  constexpr bool operator!=(Ipv4Addr o) const noexcept {
    return m_addr != o.m_addr;
  }
  constexpr bool operator<(Ipv4Addr o) const noexcept {
    return m_addr < o.m_addr;
  }

private:
  uint32_t m_addr;

  constexpr Ipv4Addr() noexcept : m_addr(0) {}
};

/* ── Ipv6Addr ──────────────────────────────────────────────────────── */

class Ipv6Addr {
public:
  static constexpr Ipv6Addr from(uint16_t s0, uint16_t s1, uint16_t s2, uint16_t s3, uint16_t s4,
                                 uint16_t s5, uint16_t s6, uint16_t s7) noexcept {
    return Ipv6Addr(s0, s1, s2, s3, s4, s5, s6, s7);
  }

  static constexpr Ipv6Addr localhost() noexcept {
    return from(0, 0, 0, 0, 0, 0, 0, 1);
  }
  static constexpr Ipv6Addr unspecified() noexcept {
    return from(0, 0, 0, 0, 0, 0, 0, 0);
  }

  static inline Result<Ipv6Addr, AddrParseError> parse(Span<const char> s) {
    if (s.is_empty()) return Result<Ipv6Addr, AddrParseError>(err, AddrParseError::InvalidIpv6);
    char   buf[48];
    size_t len = s.size() < 47 ? s.size() : 47;
    memcpy(buf, s.data(), len);
    buf[len] = '\0';
    struct in6_addr addr;
    if (inet_pton(AF_INET6, buf, &addr) != 1)
      return Result<Ipv6Addr, AddrParseError>(err, AddrParseError::InvalidIpv6);
    return Result<Ipv6Addr, AddrParseError>(
      ok, Ipv6Addr(static_cast<uint16_t>((addr.s6_addr[0] << 8) | addr.s6_addr[1]),
                   static_cast<uint16_t>((addr.s6_addr[2] << 8) | addr.s6_addr[3]),
                   static_cast<uint16_t>((addr.s6_addr[4] << 8) | addr.s6_addr[5]),
                   static_cast<uint16_t>((addr.s6_addr[6] << 8) | addr.s6_addr[7]),
                   static_cast<uint16_t>((addr.s6_addr[8] << 8) | addr.s6_addr[9]),
                   static_cast<uint16_t>((addr.s6_addr[10] << 8) | addr.s6_addr[11]),
                   static_cast<uint16_t>((addr.s6_addr[12] << 8) | addr.s6_addr[13]),
                   static_cast<uint16_t>((addr.s6_addr[14] << 8) | addr.s6_addr[15])));
  }

  static inline Result<Ipv6Addr, AddrParseError> parse(const char *s) {
    return parse(Span<const char>(s, strlen(s)));
  }

  static inline Result<Ipv6Addr, AddrParseError> parse(const std::string &s) {
    return parse(Span<const char>(s.data(), s.size()));
  }

  struct Segments {
    uint16_t data[8];
  };

  inline Segments segments() const noexcept {
    Segments segs;
    for (int i = 0; i < 8; ++i)
      segs.data[i] = static_cast<uint16_t>((m_octets[i * 2] << 8) | m_octets[i * 2 + 1]);
    return segs;
  }

  struct Octets {
    uint8_t data[16];
  };

  constexpr Octets octets() const noexcept {
    return Octets{{m_octets[0], m_octets[1], m_octets[2], m_octets[3], m_octets[4], m_octets[5],
                   m_octets[6], m_octets[7], m_octets[8], m_octets[9], m_octets[10], m_octets[11],
                   m_octets[12], m_octets[13], m_octets[14], m_octets[15]}};
  }

  constexpr bool is_loopback() const noexcept {
    return m_octets[15] == 1 &&
           (m_octets[0] | m_octets[1] | m_octets[2] | m_octets[3] | m_octets[4] | m_octets[5] |
            m_octets[6] | m_octets[7] | m_octets[8] | m_octets[9] | m_octets[10] | m_octets[11] |
            m_octets[12] | m_octets[13] | m_octets[14]) == 0;
  }

  constexpr bool is_unspecified() const noexcept {
    return (m_octets[0] | m_octets[1] | m_octets[2] | m_octets[3] | m_octets[4] | m_octets[5] |
            m_octets[6] | m_octets[7] | m_octets[8] | m_octets[9] | m_octets[10] | m_octets[11] |
            m_octets[12] | m_octets[13] | m_octets[14] | m_octets[15]) == 0;
  }

  constexpr bool is_multicast() const noexcept {
    return m_octets[0] == 0xFF;
  }

  constexpr bool is_ipv4_mapped() const noexcept {
    return m_octets[0] == 0 && m_octets[1] == 0 && m_octets[2] == 0 && m_octets[3] == 0 &&
           m_octets[4] == 0 && m_octets[5] == 0 && m_octets[6] == 0 && m_octets[7] == 0 &&
           m_octets[8] == 0 && m_octets[9] == 0 && m_octets[10] == 0xFF && m_octets[11] == 0xFF;
  }

  inline Option<Ipv4Addr> to_ipv4() const noexcept {
    if (!is_ipv4_mapped()) return none;
    return Some(Ipv4Addr::from(m_octets[12], m_octets[13], m_octets[14], m_octets[15]));
  }

  inline std::string to_string() const {
    char            buf[48];
    struct in6_addr addr;
    memcpy(&addr, m_octets, 16);
    inet_ntop(AF_INET6, &addr, buf, sizeof(buf));
    return std::string(buf);
  }

  bool operator==(Ipv6Addr o) const noexcept {
    return memcmp(m_octets, o.m_octets, 16) == 0;
  }
  bool operator!=(Ipv6Addr o) const noexcept {
    return !(*this == o);
  }
  bool operator<(Ipv6Addr o) const noexcept {
    return memcmp(m_octets, o.m_octets, 16) < 0;
  }

private:
  uint8_t m_octets[16];

  explicit constexpr Ipv6Addr(uint16_t s0, uint16_t s1, uint16_t s2, uint16_t s3, uint16_t s4,
                              uint16_t s5, uint16_t s6, uint16_t s7) noexcept
      : m_octets{static_cast<uint8_t>(s0 >> 8), static_cast<uint8_t>(s0),
                 static_cast<uint8_t>(s1 >> 8), static_cast<uint8_t>(s1),
                 static_cast<uint8_t>(s2 >> 8), static_cast<uint8_t>(s2),
                 static_cast<uint8_t>(s3 >> 8), static_cast<uint8_t>(s3),
                 static_cast<uint8_t>(s4 >> 8), static_cast<uint8_t>(s4),
                 static_cast<uint8_t>(s5 >> 8), static_cast<uint8_t>(s5),
                 static_cast<uint8_t>(s6 >> 8), static_cast<uint8_t>(s6),
                 static_cast<uint8_t>(s7 >> 8), static_cast<uint8_t>(s7)} {}

  friend class Ipv4Addr;
};

/* ── Ipv4Addr deferred methods ─────────────────────────────────────── */

inline Ipv6Addr Ipv4Addr::to_ipv6_mapped() const noexcept {
  return Ipv6Addr::from(0, 0, 0, 0, 0, 0xFFFF, static_cast<uint16_t>(m_addr >> 16),
                        static_cast<uint16_t>(m_addr));
}

inline Ipv6Addr Ipv4Addr::to_ipv6_compatible() const noexcept {
  return Ipv6Addr::from(0, 0, 0, 0, 0, 0, static_cast<uint16_t>(m_addr >> 16),
                        static_cast<uint16_t>(m_addr));
}

/* ── IpAddr ────────────────────────────────────────────────────────── */

class IpAddr {
public:
  static inline IpAddr from(Ipv4Addr addr) noexcept {
    return IpAddr(Variant<Ipv4Addr, Ipv6Addr>(addr));
  }
  static inline IpAddr from(Ipv6Addr addr) noexcept {
    return IpAddr(Variant<Ipv4Addr, Ipv6Addr>(addr));
  }

  static inline Result<IpAddr, AddrParseError> parse(Span<const char> s) {
    auto v4 = Ipv4Addr::parse(s);
    if (v4.is_ok()) return Result<IpAddr, AddrParseError>(ok, IpAddr::from(v4.unwrap()));
    auto v6 = Ipv6Addr::parse(s);
    if (v6.is_ok()) return Result<IpAddr, AddrParseError>(ok, IpAddr::from(v6.unwrap()));
    return Result<IpAddr, AddrParseError>(err, AddrParseError::InvalidFormat);
  }

  static inline Result<IpAddr, AddrParseError> parse(const char *s) {
    return parse(Span<const char>(s, strlen(s)));
  }

  static inline Result<IpAddr, AddrParseError> parse(const std::string &s) {
    return parse(Span<const char>(s.data(), s.size()));
  }

  bool is_ipv4() const noexcept {
    return m_data.index() == 0;
  }
  bool is_ipv6() const noexcept {
    return m_data.index() == 1;
  }

  Ipv4Addr &as_ipv4() {
    return m_data.get<Ipv4Addr>();
  }
  const Ipv4Addr &as_ipv4() const {
    return m_data.get<Ipv4Addr>();
  }
  Ipv6Addr &as_ipv6() {
    return m_data.get<Ipv6Addr>();
  }
  const Ipv6Addr &as_ipv6() const {
    return m_data.get<Ipv6Addr>();
  }

  Option<Ipv4Addr> ipv4() const {
    if (is_ipv4()) return Some(m_data.get_unchecked<Ipv4Addr>());
    return none;
  }
  Option<Ipv6Addr> ipv6() const {
    if (is_ipv6()) return Some(m_data.get_unchecked<Ipv6Addr>());
    return none;
  }

  inline std::string to_string() const {
    return is_ipv4() ? as_ipv4().to_string() : as_ipv6().to_string();
  }

  bool operator==(const IpAddr &o) const noexcept {
    if (m_data.index() != o.m_data.index()) return false;
    return is_ipv4() ? as_ipv4() == o.as_ipv4() : as_ipv6() == o.as_ipv6();
  }
  bool operator!=(const IpAddr &o) const noexcept {
    return !(*this == o);
  }

private:
  Variant<Ipv4Addr, Ipv6Addr> m_data;

  explicit IpAddr(Variant<Ipv4Addr, Ipv6Addr> data) noexcept : m_data(std::move(data)) {}
};

/* ── SocketAddrV4 ──────────────────────────────────────────────────── */

class SocketAddrV4 {
public:
  static constexpr SocketAddrV4 from(Ipv4Addr ip, uint16_t port) noexcept {
    return SocketAddrV4(ip, port);
  }

  static inline Result<SocketAddrV4, AddrParseError> parse(Span<const char> s) {
    const char *data  = s.data();
    int         len   = static_cast<int>(s.size());
    int         colon = -1;
    for (int i = len - 1; i >= 0; --i) {
      if (data[i] == ':') {
        colon = i;
        break;
      }
    }
    if (colon <= 0) return Result<SocketAddrV4, AddrParseError>(err, AddrParseError::InvalidFormat);

    auto ip = Ipv4Addr::parse(Span<const char>(data, static_cast<size_t>(colon)));
    if (ip.is_err()) return Result<SocketAddrV4, AddrParseError>(err, AddrParseError::InvalidIpv4);

    char port_buf[6];
    int  port_len = len - colon - 1;
    if (port_len <= 0 || port_len > 5)
      return Result<SocketAddrV4, AddrParseError>(err, AddrParseError::InvalidPort);
    memcpy(port_buf, data + colon + 1, static_cast<size_t>(port_len));
    port_buf[port_len] = '\0';
    char *end;
    long  port = strtol(port_buf, &end, 10);
    if (*end != '\0' || port < 0 || port > 65535)
      return Result<SocketAddrV4, AddrParseError>(err, AddrParseError::InvalidPort);

    return Result<SocketAddrV4, AddrParseError>(
      ok, SocketAddrV4::from(ip.unwrap(), static_cast<uint16_t>(port)));
  }

  static inline Result<SocketAddrV4, AddrParseError> parse(const char *s) {
    return parse(Span<const char>(s, strlen(s)));
  }
  static inline Result<SocketAddrV4, AddrParseError> parse(const std::string &s) {
    return parse(Span<const char>(s.data(), s.size()));
  }

  constexpr Ipv4Addr ip() const noexcept {
    return m_ip;
  }
  constexpr uint16_t port() const noexcept {
    return m_port;
  }

  void set_ip(Ipv4Addr ip) noexcept {
    m_ip = ip;
  }
  void set_port(uint16_t port) noexcept {
    m_port = port;
  }

  inline std::string to_string() const {
    std::string s = m_ip.to_string();
    char        port_buf[8];
    snprintf(port_buf, sizeof(port_buf), ":%u", m_port);
    s += port_buf;
    return s;
  }

  constexpr bool operator==(SocketAddrV4 o) const noexcept {
    return m_ip == o.m_ip && m_port == o.m_port;
  }
  constexpr bool operator!=(SocketAddrV4 o) const noexcept {
    return !(*this == o);
  }

private:
  Ipv4Addr m_ip;
  uint16_t m_port;

  constexpr SocketAddrV4(Ipv4Addr ip, uint16_t port) noexcept : m_ip(ip), m_port(port) {}
};

/* ── SocketAddrV6 ──────────────────────────────────────────────────── */

class SocketAddrV6 {
public:
  static constexpr SocketAddrV6 from(Ipv6Addr ip, uint16_t port, uint32_t flowinfo,
                                     uint32_t scope_id) noexcept {
    return SocketAddrV6(ip, port, flowinfo, scope_id);
  }

  static inline Result<SocketAddrV6, AddrParseError> parse(Span<const char> s) {
    const char *data = s.data();
    int         len  = static_cast<int>(s.size());
    if (len < 4 || data[0] != '[')
      return Result<SocketAddrV6, AddrParseError>(err, AddrParseError::InvalidFormat);

    int delim = -1;
    for (int i = 1; i < len - 1; ++i) {
      if (data[i] == ']' && i + 1 < len && data[i + 1] == ':') {
        delim = i;
        break;
      }
    }
    if (delim < 0) return Result<SocketAddrV6, AddrParseError>(err, AddrParseError::InvalidFormat);

    auto ip = Ipv6Addr::parse(Span<const char>(data + 1, static_cast<size_t>(delim - 1)));
    if (ip.is_err()) return Result<SocketAddrV6, AddrParseError>(err, AddrParseError::InvalidIpv6);

    char port_buf[6];
    int  port_len = len - delim - 2;
    if (port_len <= 0 || port_len > 5)
      return Result<SocketAddrV6, AddrParseError>(err, AddrParseError::InvalidPort);
    memcpy(port_buf, data + delim + 2, static_cast<size_t>(port_len));
    port_buf[port_len] = '\0';
    char *end;
    long  port = strtol(port_buf, &end, 10);
    if (*end != '\0' || port < 0 || port > 65535)
      return Result<SocketAddrV6, AddrParseError>(err, AddrParseError::InvalidPort);

    return Result<SocketAddrV6, AddrParseError>(
      ok, SocketAddrV6::from(ip.unwrap(), static_cast<uint16_t>(port), 0, 0));
  }

  static inline Result<SocketAddrV6, AddrParseError> parse(const char *s) {
    return parse(Span<const char>(s, strlen(s)));
  }
  static inline Result<SocketAddrV6, AddrParseError> parse(const std::string &s) {
    return parse(Span<const char>(s.data(), s.size()));
  }

  constexpr Ipv6Addr ip() const noexcept {
    return m_ip;
  }
  constexpr uint16_t port() const noexcept {
    return m_port;
  }
  constexpr uint32_t flowinfo() const noexcept {
    return m_flowinfo;
  }
  constexpr uint32_t scope_id() const noexcept {
    return m_scope_id;
  }

  void set_ip(Ipv6Addr ip) noexcept {
    m_ip = ip;
  }
  void set_port(uint16_t port) noexcept {
    m_port = port;
  }
  void set_flowinfo(uint32_t fi) noexcept {
    m_flowinfo = fi;
  }
  void set_scope_id(uint32_t sid) noexcept {
    m_scope_id = sid;
  }

  inline std::string to_string() const {
    std::string s = "[";
    s += m_ip.to_string();
    char port_buf[8];
    snprintf(port_buf, sizeof(port_buf), "]:%u", m_port);
    s += port_buf;
    return s;
  }

  bool operator==(SocketAddrV6 o) const noexcept {
    return m_ip == o.m_ip && m_port == o.m_port && m_flowinfo == o.m_flowinfo &&
           m_scope_id == o.m_scope_id;
  }
  bool operator!=(SocketAddrV6 o) const noexcept {
    return !(*this == o);
  }

private:
  Ipv6Addr m_ip;
  uint16_t m_port;
  uint32_t m_flowinfo;
  uint32_t m_scope_id;

  constexpr SocketAddrV6(Ipv6Addr ip, uint16_t port, uint32_t flowinfo, uint32_t scope_id) noexcept
      : m_ip(ip), m_port(port), m_flowinfo(flowinfo), m_scope_id(scope_id) {}
};

/* ── SocketAddr ────────────────────────────────────────────────────── */

class SocketAddr {
public:
  static inline SocketAddr from(SocketAddrV4 addr) noexcept {
    return SocketAddr(Variant<SocketAddrV4, SocketAddrV6>(addr));
  }
  static inline SocketAddr from(SocketAddrV6 addr) noexcept {
    return SocketAddr(Variant<SocketAddrV4, SocketAddrV6>(addr));
  }

  static inline SocketAddr unspecified() noexcept {
    return SocketAddr::from(SocketAddrV4::from(Ipv4Addr::unspecified(), 0));
  }

  static inline Result<SocketAddr, AddrParseError> parse(Span<const char> s) {
    if (s.is_empty()) return Result<SocketAddr, AddrParseError>(err, AddrParseError::InvalidFormat);
    if (s.data()[0] == '[') {
      auto v6 = SocketAddrV6::parse(s);
      if (v6.is_ok()) return Result<SocketAddr, AddrParseError>(ok, SocketAddr::from(v6.unwrap()));
      return Result<SocketAddr, AddrParseError>(err, v6.unwrap_err());
    }
    auto v4 = SocketAddrV4::parse(s);
    if (v4.is_ok()) return Result<SocketAddr, AddrParseError>(ok, SocketAddr::from(v4.unwrap()));
    return Result<SocketAddr, AddrParseError>(err, v4.unwrap_err());
  }

  static inline Result<SocketAddr, AddrParseError> parse(const char *s) {
    return parse(Span<const char>(s, strlen(s)));
  }
  static inline Result<SocketAddr, AddrParseError> parse(const std::string &s) {
    return parse(Span<const char>(s.data(), s.size()));
  }

  bool is_ipv4() const noexcept {
    return m_data.index() == 0;
  }
  bool is_ipv6() const noexcept {
    return m_data.index() == 1;
  }

  SocketAddrV4 &as_v4() {
    return m_data.get<SocketAddrV4>();
  }
  const SocketAddrV4 &as_v4() const {
    return m_data.get<SocketAddrV4>();
  }
  SocketAddrV6 &as_v6() {
    return m_data.get<SocketAddrV6>();
  }
  const SocketAddrV6 &as_v6() const {
    return m_data.get<SocketAddrV6>();
  }

  inline IpAddr   ip() const;
  inline uint16_t port() const;

  Option<SocketAddrV4> v4() const {
    if (is_ipv4()) return Some(m_data.get_unchecked<SocketAddrV4>());
    return none;
  }
  Option<SocketAddrV6> v6() const {
    if (is_ipv6()) return Some(m_data.get_unchecked<SocketAddrV6>());
    return none;
  }

  inline std::string to_string() const {
    return is_ipv4() ? as_v4().to_string() : as_v6().to_string();
  }

  bool operator==(const SocketAddr &o) const noexcept {
    if (m_data.index() != o.m_data.index()) return false;
    return is_ipv4() ? as_v4() == o.as_v4() : as_v6() == o.as_v6();
  }
  bool operator!=(const SocketAddr &o) const noexcept {
    return !(*this == o);
  }

  /* sockaddr interop */
  static inline Option<SocketAddr> from_sockaddr(const struct sockaddr *sa, socklen_t len);
  inline void to_sockaddr(struct sockaddr_storage *out, socklen_t *out_len) const;

private:
  Variant<SocketAddrV4, SocketAddrV6> m_data;

  explicit SocketAddr(Variant<SocketAddrV4, SocketAddrV6> data) noexcept
      : m_data(std::move(data)) {}
};

/* ── SocketAddr deferred methods ───────────────────────────────────── */

inline IpAddr SocketAddr::ip() const {
  return is_ipv4() ? IpAddr::from(as_v4().ip()) : IpAddr::from(as_v6().ip());
}

inline uint16_t SocketAddr::port() const {
  return is_ipv4() ? as_v4().port() : as_v6().port();
}

inline Option<SocketAddr> SocketAddr::from_sockaddr(const struct sockaddr *sa, socklen_t len) {
  if (sa->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
    auto          *in = reinterpret_cast<const struct sockaddr_in *>(sa);
    const uint8_t *p  = reinterpret_cast<const uint8_t *>(&in->sin_addr);
    return Some(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::from(p[0], p[1], p[2], p[3]), ntohs(in->sin_port))));
  }
  if (sa->sa_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
    auto *in6 = reinterpret_cast<const struct sockaddr_in6 *>(sa);
    return Some(SocketAddr::from(SocketAddrV6::from(
      Ipv6Addr::from(
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[0] << 8) | in6->sin6_addr.s6_addr[1]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[2] << 8) | in6->sin6_addr.s6_addr[3]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[4] << 8) | in6->sin6_addr.s6_addr[5]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[6] << 8) | in6->sin6_addr.s6_addr[7]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[8] << 8) | in6->sin6_addr.s6_addr[9]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[10] << 8) | in6->sin6_addr.s6_addr[11]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[12] << 8) | in6->sin6_addr.s6_addr[13]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[14] << 8) | in6->sin6_addr.s6_addr[15])),
      ntohs(in6->sin6_port), ntohl(in6->sin6_flowinfo), ntohl(in6->sin6_scope_id))));
  }
  return none;
}

inline void SocketAddr::to_sockaddr(struct sockaddr_storage *out, socklen_t *out_len) const {
  memset(out, 0, sizeof(*out));
  if (is_ipv4()) {
    auto    *in    = reinterpret_cast<struct sockaddr_in *>(out);
    Ipv4Addr ip    = as_v4().ip();
    in->sin_family = AF_INET;
    in->sin_port   = htons(as_v4().port());
    uint8_t *p     = reinterpret_cast<uint8_t *>(&in->sin_addr);
    p[0]           = static_cast<uint8_t>(ip[0]);
    p[1]           = static_cast<uint8_t>(ip[1]);
    p[2]           = static_cast<uint8_t>(ip[2]);
    p[3]           = static_cast<uint8_t>(ip[3]);
    *out_len       = sizeof(struct sockaddr_in);
  } else {
    auto    *in6       = reinterpret_cast<struct sockaddr_in6 *>(out);
    Ipv6Addr ip        = as_v6().ip();
    in6->sin6_family   = AF_INET6;
    in6->sin6_port     = htons(as_v6().port());
    in6->sin6_flowinfo = htonl(as_v6().flowinfo());
    in6->sin6_scope_id = htonl(as_v6().scope_id());
    memcpy(&in6->sin6_addr, &ip, 16);
    *out_len = sizeof(struct sockaddr_in6);
  }
}

} // namespace net
} // namespace xpp

#endif // XPP_NET_ADDR_H
