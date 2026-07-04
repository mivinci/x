/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * addr_test.cpp — Tests for xpp::net address types.
 */
#include <gtest/gtest.h>
#include <xpp/net/addr.h>

using namespace xpp::net;

/* ── Ipv4Addr ──────────────────────────────────────────────────────── */

TEST(AddrTest, Ipv4FromOctets) {
  auto addr = Ipv4Addr::from(127, 0, 0, 1);
  EXPECT_TRUE(addr.is_loopback());
  EXPECT_EQ(addr.to_string(), "127.0.0.1");
}

TEST(AddrTest, Ipv4Parse) {
  auto r = Ipv4Addr::parse("192.168.1.1");
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap().to_string(), "192.168.1.1");
  EXPECT_TRUE(r.unwrap().is_private());
}

TEST(AddrTest, Ipv4ParseInvalid) {
  EXPECT_TRUE(Ipv4Addr::parse("256.1.1.1").is_err());
  EXPECT_TRUE(Ipv4Addr::parse("not an ip").is_err());
  EXPECT_TRUE(Ipv4Addr::parse("").is_err());
}

TEST(AddrTest, Ipv4Properties) {
  EXPECT_TRUE(Ipv4Addr::localhost().is_loopback());
  EXPECT_TRUE(Ipv4Addr::unspecified().is_unspecified());
  EXPECT_TRUE(Ipv4Addr::broadcast().is_broadcast());
  EXPECT_TRUE(Ipv4Addr::from(224, 0, 0, 1).is_multicast());
  EXPECT_TRUE(Ipv4Addr::from(169, 254, 1, 1).is_link_local());
  EXPECT_TRUE(Ipv4Addr::from(10, 0, 0, 1).is_private());
  EXPECT_TRUE(Ipv4Addr::from(172, 16, 0, 1).is_private());
  EXPECT_TRUE(Ipv4Addr::from(192, 168, 0, 1).is_private());
}

TEST(AddrTest, Ipv4Octets) {
  auto addr = Ipv4Addr::from(1, 2, 3, 4);
  auto o    = addr.octets();
  EXPECT_EQ(o.data[0], 1);
  EXPECT_EQ(o.data[1], 2);
  EXPECT_EQ(o.data[2], 3);
  EXPECT_EQ(o.data[3], 4);
  EXPECT_EQ(addr[0], 1);
  EXPECT_EQ(addr[3], 4);
}

TEST(AddrTest, Ipv4ToIpv6Mapped) {
  auto v4 = Ipv4Addr::from(192, 168, 1, 1);
  auto v6 = v4.to_ipv6_mapped();
  EXPECT_TRUE(v6.is_ipv4_mapped());
  auto back = v6.to_ipv4();
  ASSERT_TRUE(back.is_some());
  EXPECT_EQ(back.unwrap(), v4);
}

/* ── Ipv6Addr ──────────────────────────────────────────────────────── */

TEST(AddrTest, Ipv6FromSegments) {
  auto addr = Ipv6Addr::from(0x2001, 0x0db8, 0, 0, 0, 0, 0, 1);
  auto segs = addr.segments();
  EXPECT_EQ(segs.data[0], 0x2001);
  EXPECT_EQ(segs.data[1], 0x0db8);
  EXPECT_EQ(segs.data[7], 1);
}

TEST(AddrTest, Ipv6Parse) {
  auto r = Ipv6Addr::parse("::1");
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is_loopback());
  EXPECT_EQ(r.unwrap().to_string(), "::1");
}

TEST(AddrTest, Ipv6ParseFull) {
  auto r = Ipv6Addr::parse("2001:db8::1");
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap().to_string(), "2001:db8::1");
}

TEST(AddrTest, Ipv6ParseInvalid) {
  EXPECT_TRUE(Ipv6Addr::parse("not an ip").is_err());
  EXPECT_TRUE(Ipv6Addr::parse("").is_err());
}

TEST(AddrTest, Ipv6Properties) {
  EXPECT_TRUE(Ipv6Addr::localhost().is_loopback());
  EXPECT_TRUE(Ipv6Addr::unspecified().is_unspecified());
  EXPECT_TRUE(Ipv6Addr::from(0xFF00, 0, 0, 0, 0, 0, 0, 0).is_multicast());
}

/* ── IpAddr ────────────────────────────────────────────────────────── */

TEST(AddrTest, IpAddrFromV4) {
  auto addr = IpAddr::from(Ipv4Addr::localhost());
  EXPECT_TRUE(addr.is_ipv4());
  EXPECT_FALSE(addr.is_ipv6());
  EXPECT_EQ(addr.to_string(), "127.0.0.1");
}

TEST(AddrTest, IpAddrFromV6) {
  auto addr = IpAddr::from(Ipv6Addr::localhost());
  EXPECT_TRUE(addr.is_ipv6());
  EXPECT_FALSE(addr.is_ipv4());
  EXPECT_EQ(addr.to_string(), "::1");
}

TEST(AddrTest, IpAddrParse) {
  auto v4 = IpAddr::parse("10.0.0.1");
  ASSERT_TRUE(v4.is_ok());
  EXPECT_TRUE(v4.unwrap().is_ipv4());

  auto v6 = IpAddr::parse("::1");
  ASSERT_TRUE(v6.is_ok());
  EXPECT_TRUE(v6.unwrap().is_ipv6());
}

/* ── SocketAddrV4 ──────────────────────────────────────────────────── */

TEST(AddrTest, SocketAddrV4Parse) {
  auto r = SocketAddrV4::parse("127.0.0.1:8080");
  ASSERT_TRUE(r.is_ok());
  auto sa = r.unwrap();
  EXPECT_EQ(sa.ip(), Ipv4Addr::localhost());
  EXPECT_EQ(sa.port(), 8080);
  EXPECT_EQ(sa.to_string(), "127.0.0.1:8080");
}

TEST(AddrTest, SocketAddrV4ParseInvalid) {
  EXPECT_TRUE(SocketAddrV4::parse("no port").is_err());
  EXPECT_TRUE(SocketAddrV4::parse("1.2.3.4:99999").is_err());
  EXPECT_TRUE(SocketAddrV4::parse("1.2.3.4:").is_err());
}

/* ── SocketAddrV6 ──────────────────────────────────────────────────── */

TEST(AddrTest, SocketAddrV6Parse) {
  auto r = SocketAddrV6::parse("[::1]:443");
  ASSERT_TRUE(r.is_ok());
  auto sa = r.unwrap();
  EXPECT_EQ(sa.ip(), Ipv6Addr::localhost());
  EXPECT_EQ(sa.port(), 443);
  EXPECT_EQ(sa.to_string(), "[::1]:443");
}

TEST(AddrTest, SocketAddrV6ParseInvalid) {
  EXPECT_TRUE(SocketAddrV6::parse("::1:443").is_err());
  EXPECT_TRUE(SocketAddrV6::parse("[::1").is_err());
  EXPECT_TRUE(SocketAddrV6::parse("[::1]:99999").is_err());
}

/* ── SocketAddr ────────────────────────────────────────────────────── */

TEST(AddrTest, SocketAddrParseV4) {
  auto r = SocketAddr::parse("0.0.0.0:80");
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is_ipv4());
  EXPECT_EQ(r.unwrap().port(), 80);
}

TEST(AddrTest, SocketAddrParseV6) {
  auto r = SocketAddr::parse("[::]:443");
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is_ipv6());
  EXPECT_EQ(r.unwrap().port(), 443);
}

TEST(AddrTest, SocketAddrUnspecified) {
  auto sa = SocketAddr::unspecified();
  EXPECT_TRUE(sa.is_ipv4());
  EXPECT_EQ(sa.port(), 0);
}

/* ── sockaddr interop ──────────────────────────────────────────────── */

TEST(AddrTest, SockaddrRoundtripV4) {
  auto sa = SocketAddr::from(SocketAddrV4::from(Ipv4Addr::from(1, 2, 3, 4), 5678));

  struct sockaddr_storage ss;
  socklen_t               len;
  sa.to_sockaddr(&ss, &len);
  ASSERT_EQ(len, sizeof(struct sockaddr_in));

  auto back = SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&ss), len);
  ASSERT_TRUE(back.is_some());
  EXPECT_EQ(back.unwrap(), sa);
}

TEST(AddrTest, SockaddrRoundtripV6) {
  auto sa = SocketAddr::from(
    SocketAddrV6::from(Ipv6Addr::from(0x2001, 0xdb8, 0, 0, 0, 0, 0, 1), 443, 0, 0));

  struct sockaddr_storage ss;
  socklen_t               len;
  sa.to_sockaddr(&ss, &len);
  ASSERT_EQ(len, sizeof(struct sockaddr_in6));

  auto back = SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&ss), len);
  ASSERT_TRUE(back.is_some());
  EXPECT_EQ(back.unwrap(), sa);
}
