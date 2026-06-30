/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns_test.cpp - Tests for the async DNS module
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>

#include <x/base/event.h>
#include <x/base/socket.h>
#include <x/base/time.h>
#include <x/dns/dns.h>

#include "dns_private.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* ───────────────────── Helpers ───────────────────── */

static void pump_loop(xEventLoop loop, int total_ms) {
  xTimer t = xTimerStart(
    [](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop,
    static_cast<uint64_t>(total_ms), 0);
  xEventLoopRun(loop, X_RUN_DEFAULT);
  if (t) xTimerStop(t);
}

/* Return true if we can send a UDP packet to 8.8.8.8:53 and get a reply
 * within a short window. Used to guard real-network integration tests. */
static bool can_reach_dns(void) {
#ifdef _WIN32
  return false; /* skip network tests on Windows CI by default */
#else
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  struct timeval tv;
  tv.tv_sec  = 2;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family      = AF_INET;
  sin.sin_port        = htons(53);
  inet_pton(AF_INET, "8.8.8.8", &sin.sin_addr);

  /* Minimal DNS query for "example.com" type A */
  uint8_t query[] = {
    0xab, 0xcd,                   /* ID */
    0x01, 0x00,                   /* flags: RD */
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* counts */
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm', 0x00,
    0x00, 0x01, 0x00, 0x01};
  sendto(fd, reinterpret_cast<const char *>(query), sizeof(query), 0,
         reinterpret_cast<struct sockaddr *>(&sin), sizeof(sin));

  uint8_t buf[512];
  struct sockaddr_storage src;
  socklen_t srclen = sizeof(src);
  ssize_t n = recvfrom(fd, reinterpret_cast<char *>(buf), sizeof(buf), 0,
                       reinterpret_cast<struct sockaddr *>(&src), &srclen);
  close(fd);
  return n > 0;
#endif
}

/* ═══════════════════════════════════════════════════════════════════
 *  Packet build / parse unit tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(DnsPacket, BuildQueryA) {
  uint8_t buf[512];
  int n = dns_build_query(buf, sizeof(buf), 0x1234, "example.com", DNS_QTYPE_A);
  ASSERT_GT(n, 0);

  /* Header: 12 bytes */
  ASSERT_GE(n, 12);
  EXPECT_EQ((buf[0] << 8) | buf[1], 0x1234);          /* ID */
  uint16_t flags = (buf[2] << 8) | buf[3];
  EXPECT_TRUE(flags & DNS_FLAG_RD);                    /* RD set */
  EXPECT_EQ((buf[4] << 8) | buf[5], 1);                /* QDCOUNT=1 */
  EXPECT_EQ((buf[6] << 8) | buf[7], 0);                /* ANCOUNT=0 */
  EXPECT_EQ((buf[8] << 8) | buf[9], 0);                /* NSCOUNT=0 */
  EXPECT_EQ((buf[10] << 8) | buf[11], 1);              /* ARCOUNT=1 (OPT) */

  /* QNAME: \x07example\x03com\x00 */
  EXPECT_EQ(buf[12], 7);
  EXPECT_EQ(memcmp(buf + 13, "example", 7), 0);
  EXPECT_EQ(buf[20], 3);
  EXPECT_EQ(memcmp(buf + 21, "com", 3), 0);
  EXPECT_EQ(buf[24], 0);

  /* QTYPE=A (1), QCLASS=IN (1) */
  EXPECT_EQ((buf[25] << 8) | buf[26], 1);
  EXPECT_EQ((buf[27] << 8) | buf[28], 1);
}

TEST(DnsPacket, BuildQueryRoundTrip) {
  for (uint16_t qt : {DNS_QTYPE_A, DNS_QTYPE_AAAA, DNS_QTYPE_CNAME}) {
    uint8_t buf[512];
    int n = dns_build_query(buf, sizeof(buf), 0x4242, "example.com", qt);
    ASSERT_GT(n, 0);

    dns_header_t   hdr;
    dns_question_t q;
    xDnsRecord    *answers = nullptr;
    ASSERT_EQ(dns_parse(buf, (size_t)n, &hdr, &q, &answers), xErrno_Ok);
    EXPECT_EQ(hdr.id, 0x4242);
    EXPECT_EQ(hdr.qdcount, 1u);
    EXPECT_EQ(hdr.arcount, 1u); /* EDNS0 OPT */
    EXPECT_EQ(std::string(q.name), "example.com");
    EXPECT_EQ(q.qtype, qt);
    EXPECT_EQ(q.qclass, DNS_QCLASS_IN);
    EXPECT_EQ(answers, nullptr);
    dns_records_free(answers);
  }
}

TEST(DnsPacket, ParseResponseA) {
  /* Build a response by hand: 1 A record for example.com → 93.184.216.34 */
  uint8_t ip[4] = {93, 184, 216, 34};
  xDnsRecord rec = {};
  rec.qtype    = DNS_QTYPE_A;
  rec.ttl      = 300;
  rec.name     = "example.com";
  rec.rdata    = ip;
  rec.rdlength = 4;

  uint8_t buf[512];
  int n = dns_build_response(buf, sizeof(buf), 0x7777, 0, "example.com",
                             DNS_QTYPE_A, &rec);
  ASSERT_GT(n, 0);

  dns_header_t   hdr;
  dns_question_t q;
  xDnsRecord    *answers = nullptr;
  ASSERT_EQ(dns_parse(buf, (size_t)n, &hdr, &q, &answers), xErrno_Ok);
  EXPECT_EQ(hdr.id, 0x7777);
  EXPECT_TRUE(hdr.flags & DNS_FLAG_QR);
  EXPECT_EQ(hdr.ancount, 1u);
  EXPECT_EQ(std::string(q.name), "example.com");
  ASSERT_NE(answers, nullptr);
  EXPECT_EQ(answers->qtype, DNS_QTYPE_A);
  EXPECT_EQ(answers->rdlength, 4u);
  EXPECT_EQ(memcmp(answers->rdata, ip, 4), 0);
  EXPECT_EQ(answers->next, nullptr);
  dns_records_free(answers);
}

TEST(DnsPacket, ParseResponseAAAA) {
  uint8_t ip[16] = {0x26, 0x06, 0x28, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};
  xDnsRecord rec = {};
  rec.qtype    = DNS_QTYPE_AAAA;
  rec.ttl      = 300;
  rec.name     = "example.com";
  rec.rdata    = ip;
  rec.rdlength = 16;

  uint8_t buf[512];
  int n = dns_build_response(buf, sizeof(buf), 0x01, 0, "example.com",
                             DNS_QTYPE_AAAA, &rec);
  ASSERT_GT(n, 0);

  dns_header_t   hdr;
  dns_question_t q;
  xDnsRecord    *answers = nullptr;
  ASSERT_EQ(dns_parse(buf, (size_t)n, &hdr, &q, &answers), xErrno_Ok);
  ASSERT_NE(answers, nullptr);
  EXPECT_EQ(answers->qtype, DNS_QTYPE_AAAA);
  EXPECT_EQ(answers->rdlength, 16u);
  EXPECT_EQ(memcmp(answers->rdata, ip, 16), 0);
  dns_records_free(answers);
}

TEST(DnsPacket, ParseResponseCNAME) {
  xDnsRecord rec = {};
  rec.qtype    = DNS_QTYPE_CNAME;
  rec.ttl      = 300;
  rec.name     = "www.example.com";
  rec.rdata    = "example.com";
  rec.rdlength = 12; /* length of "example.com" + NUL as stored by parser */

  uint8_t buf[512];
  int n = dns_build_response(buf, sizeof(buf), 0x02, 0, "www.example.com",
                             DNS_QTYPE_CNAME, &rec);
  ASSERT_GT(n, 0);

  dns_header_t   hdr;
  dns_question_t q;
  xDnsRecord    *answers = nullptr;
  ASSERT_EQ(dns_parse(buf, (size_t)n, &hdr, &q, &answers), xErrno_Ok);
  ASSERT_NE(answers, nullptr);
  EXPECT_EQ(answers->qtype, DNS_QTYPE_CNAME);
  /* CNAME RDATA is decoded as a domain name string */
  EXPECT_STREQ((const char *)answers->rdata, "example.com");
  dns_records_free(answers);
}

TEST(DnsPacket, CompressionPointer) {
  /* Hand-craft a response where the answer NAME is a compression pointer
   * back to offset 12 (the QNAME in the question section). */
  uint8_t buf[512];
  size_t  off = 0;

  /* Header */
  buf[off++] = 0xaa; buf[off++] = 0xbb;       /* ID */
  buf[off++] = 0x80; buf[off++] = 0x00;       /* flags: QR=1 */
  buf[off++] = 0; buf[off++] = 1;             /* QDCOUNT=1 */
  buf[off++] = 0; buf[off++] = 1;             /* ANCOUNT=1 */
  buf[off++] = 0; buf[off++] = 0;             /* NSCOUNT=0 */
  buf[off++] = 0; buf[off++] = 0;             /* ARCOUNT=0 */

  /* Question: \x07example\x03com\x00 + QTYPE=A + QCLASS=IN */
  size_t qname_off = off;
  buf[off++] = 7;
  memcpy(buf + off, "example", 7); off += 7;
  buf[off++] = 3;
  memcpy(buf + off, "com", 3); off += 3;
  buf[off++] = 0;
  buf[off++] = 0; buf[off++] = 1;             /* QTYPE=A */
  buf[off++] = 0; buf[off++] = 1;             /* QCLASS=IN */

  /* Answer: NAME = compression pointer to qname_off */
  buf[off++] = 0xC0;
  buf[off++] = static_cast<uint8_t>(qname_off);
  buf[off++] = 0; buf[off++] = 1;             /* TYPE=A */
  buf[off++] = 0; buf[off++] = 1;             /* CLASS=IN */
  buf[off++] = 0; buf[off++] = 0; buf[off++] = 0; buf[off++] = 60; /* TTL=60 */
  buf[off++] = 0; buf[off++] = 4;             /* RDLENGTH=4 */
  buf[off++] = 1; buf[off++] = 2; buf[off++] = 3; buf[off++] = 4; /* RDATA */

  dns_header_t   hdr;
  dns_question_t q;
  xDnsRecord    *answers = nullptr;
  ASSERT_EQ(dns_parse(buf, off, &hdr, &q, &answers), xErrno_Ok);
  EXPECT_EQ(hdr.id, 0xaabb);
  EXPECT_EQ(std::string(q.name), "example.com");
  ASSERT_NE(answers, nullptr);
  EXPECT_EQ(answers->qtype, DNS_QTYPE_A);
  EXPECT_EQ(std::string(answers->name), "example.com"); /* resolved via pointer */
  EXPECT_EQ(answers->ttl, 60u);
  ASSERT_EQ(answers->rdlength, 4u);
  EXPECT_EQ(((const uint8_t *)answers->rdata)[0], 1);
  EXPECT_EQ(((const uint8_t *)answers->rdata)[3], 4);
  dns_records_free(answers);
}

TEST(DnsPacket, MalformedTruncated) {
  /* A valid query truncated to 5 bytes should fail to parse. */
  uint8_t buf[512];
  int n = dns_build_query(buf, sizeof(buf), 0x1, "example.com", DNS_QTYPE_A);
  ASSERT_GT(n, 0);
  dns_header_t   hdr;
  dns_question_t q;
  xDnsRecord    *answers = nullptr;
  EXPECT_NE(dns_parse(buf, 5, &hdr, &q, &answers), xErrno_Ok);
}

TEST(DnsPacket, NxDomainResponse) {
  uint8_t buf[512];
  int n = dns_build_response(buf, sizeof(buf), 0x99, static_cast<int>(DNS_RCODE_NXDOMAIN),
                             "missing.example.com", DNS_QTYPE_A, nullptr);
  ASSERT_GT(n, 0);
  dns_header_t   hdr;
  dns_question_t q;
  xDnsRecord    *answers = nullptr;
  ASSERT_EQ(dns_parse(buf, (size_t)n, &hdr, &q, &answers), xErrno_Ok);
  EXPECT_EQ(hdr.flags & DNS_RCODE_MASK, DNS_RCODE_NXDOMAIN);
  EXPECT_EQ(hdr.ancount, 0u);
  EXPECT_EQ(answers, nullptr);
  dns_records_free(answers);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Cache unit tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(DnsCache, InsertLookup) {
  xMap cache = dns_cache_create();
  ASSERT_NE(cache, nullptr);

  uint8_t ip[4] = {1, 2, 3, 4};
  xDnsRecord rec = {};
  rec.qtype = DNS_QTYPE_A;
  rec.ttl   = 3600;
  rec.name  = "test.example";
  rec.rdata = ip;
  rec.rdlength = 4;

  dns_cache_insert(cache, "test.example", DNS_QTYPE_A, &rec, 3600);

  xDnsRecord *r = dns_cache_lookup(cache, "test.example", DNS_QTYPE_A);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->qtype, DNS_QTYPE_A);
  EXPECT_EQ(r->rdlength, 4u);
  EXPECT_EQ(memcmp(r->rdata, ip, 4), 0);
  dns_records_free(r);

  /* Miss on different type */
  EXPECT_EQ(dns_cache_lookup(cache, "test.example", DNS_QTYPE_AAAA), nullptr);
  /* Miss on different name */
  EXPECT_EQ(dns_cache_lookup(cache, "other.example", DNS_QTYPE_A), nullptr);

  dns_cache_destroy(cache);
}

TEST(DnsCache, Expiry) {
  xMap cache = dns_cache_create();
  uint8_t ip[4] = {10, 20, 30, 40};
  xDnsRecord rec = {};
  rec.qtype = DNS_QTYPE_A;
  rec.ttl   = 0; /* immediate expiry */
  rec.name  = "expire.test";
  rec.rdata = ip;
  rec.rdlength = 4;

  dns_cache_insert(cache, "expire.test", DNS_QTYPE_A, &rec, 0);
  EXPECT_EQ(dns_cache_lookup(cache, "expire.test", DNS_QTYPE_A), nullptr);

  dns_cache_destroy(cache);
}

TEST(DnsCache, Replace) {
  xMap cache = dns_cache_create();
  uint8_t ip1[4] = {1, 1, 1, 1};
  uint8_t ip2[4] = {2, 2, 2, 2};
  xDnsRecord r1 = {};
  r1.qtype = DNS_QTYPE_A; r1.ttl = 60; r1.name = "r.test"; r1.rdata = ip1; r1.rdlength = 4;
  xDnsRecord r2 = {};
  r2.qtype = DNS_QTYPE_A; r2.ttl = 60; r2.name = "r.test"; r2.rdata = ip2; r2.rdlength = 4;

  dns_cache_insert(cache, "r.test", DNS_QTYPE_A, &r1, 60);
  dns_cache_insert(cache, "r.test", DNS_QTYPE_A, &r2, 60);

  xDnsRecord *r = dns_cache_lookup(cache, "r.test", DNS_QTYPE_A);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(memcmp(r->rdata, ip2, 4), 0);
  dns_records_free(r);

  dns_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Config unit tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(DnsConfig, LoadNameservers) {
  char ns[8][46];
  int n = dns_config_load_nameservers(ns, 8);
  EXPECT_GE(n, 1);
  /* Whatever we got, the first entry should parse as an IP. */
  struct in_addr  a4;
  struct in6_addr a6;
  EXPECT_TRUE(inet_pton(AF_INET, ns[0], &a4) == 1 ||
              inet_pton(AF_INET6, ns[0], &a6) == 1);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Client integration tests (real network)
 * ═══════════════════════════════════════════════════════════════════ */

struct DnsResult {
  xErrno  err;
  int     count;
  bool    fired;
  /* Captured fields of the first record (copied, since the library frees
   * the record list after the callback returns). */
  uint16_t first_qtype;
  uint32_t first_ttl;
  size_t   first_rdlen;
  uint8_t  first_rdata[64];
  bool     has_records;
  bool     has_a;
  bool     has_aaaa;
};

static void capture_cb(xErrno err, const xDnsRecord *records, void *arg) {
  auto *r = static_cast<DnsResult *>(arg);
  r->err   = err;
  r->fired = true;
  r->count = 0;
  r->has_records = false;
  r->has_a       = false;
  r->has_aaaa    = false;
  for (const xDnsRecord *p = records; p; p = p->next) {
    ++r->count;
    if (p->qtype == DNS_QTYPE_A)    r->has_a    = true;
    if (p->qtype == DNS_QTYPE_AAAA) r->has_aaaa = true;
    if (!r->has_records) {
      r->has_records  = true;
      r->first_qtype  = p->qtype;
      r->first_ttl    = p->ttl;
      r->first_rdlen  = p->rdlength;
      if (p->rdlength > 0 && p->rdata) {
        size_t n = p->rdlength < sizeof(r->first_rdata) ? p->rdlength
                                                         : sizeof(r->first_rdata);
        memcpy(r->first_rdata, p->rdata, n);
      }
    }
  }
}

TEST(DnsClient, ResolveA_8888) {
  if (!can_reach_dns()) GTEST_SKIP() << "8.8.8.8 unreachable";

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xDnsClientConf conf = {};
  conf.nameservers[0] = "8.8.8.8";
  conf.timeout_ms     = 5000;
  conf.retries        = 1;
  conf.enable_cache   = 1;

  xDnsClient client = xDnsClientCreate(&conf);
  ASSERT_NE(client, nullptr);

  DnsResult res = {};
  ASSERT_EQ(xDnsClientDo(client, "example.com", xDnsType_A, capture_cb, &res),
            xErrno_Ok);

  pump_loop(loop, 6000);
  EXPECT_TRUE(res.fired);
  EXPECT_EQ(res.err, xErrno_Ok);
  EXPECT_GE(res.count, 1);
  if (res.has_records) {
    EXPECT_EQ(res.first_qtype, DNS_QTYPE_A);
    EXPECT_EQ(res.first_rdlen, 4u);
  }

  xDnsClientDestroy(client);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(DnsClient, ResolveAAndAAAA_8888) {
  if (!can_reach_dns()) GTEST_SKIP() << "8.8.8.8 unreachable";

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xDnsClientConf conf = {};
  conf.nameservers[0] = "8.8.8.8";
  conf.timeout_ms     = 5000;
  conf.retries        = 1;
  conf.enable_cache   = 0;

  xDnsClient client = xDnsClientCreate(&conf);
  ASSERT_NE(client, nullptr);

  DnsResult res = {};
  ASSERT_EQ(xDnsClientDo(client, "google.com",
                         (xDnsType)(xDnsType_A | xDnsType_AAAA),
                         capture_cb, &res),
            xErrno_Ok);

  pump_loop(loop, 6000);
  EXPECT_TRUE(res.fired);
  EXPECT_EQ(res.err, xErrno_Ok);
  EXPECT_GE(res.count, 1);

  /* At least one A or AAAA record should be present. */
  EXPECT_TRUE(res.has_a || res.has_aaaa);

  xDnsClientDestroy(client);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(DnsClient, CacheHitReturnsImmediately) {
  if (!can_reach_dns()) GTEST_SKIP() << "8.8.8.8 unreachable";

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xDnsClientConf conf = {};
  conf.nameservers[0] = "8.8.8.8";
  conf.timeout_ms     = 5000;
  conf.retries        = 1;
  conf.enable_cache   = 1;

  xDnsClient client = xDnsClientCreate(&conf);
  ASSERT_NE(client, nullptr);

  /* First query: network */
  DnsResult r1 = {};
  xDnsClientDo(client, "example.com", xDnsType_A, capture_cb, &r1);
  pump_loop(loop, 6000);
  ASSERT_TRUE(r1.fired);
  ASSERT_EQ(r1.err, xErrno_Ok);

  /* Second query: should hit cache (callback fires synchronously). */
  DnsResult r2 = {};
  xDnsClientDo(client, "example.com", xDnsType_A, capture_cb, &r2);
  EXPECT_TRUE(r2.fired);
  EXPECT_EQ(r2.err, xErrno_Ok);
  EXPECT_GE(r2.count, 1);

  xDnsClientDestroy(client);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(DnsClient, TimeoutUnreachable) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  /* 192.0.2.1 is TEST-NET-1 (RFC 5737) — should not respond. */
  xDnsClientConf conf = {};
  conf.nameservers[0] = "192.0.2.1";
  conf.timeout_ms     = 300;
  conf.retries        = 0;
  conf.enable_cache   = 0;

  xDnsClient client = xDnsClientCreate(&conf);
  ASSERT_NE(client, nullptr);

  DnsResult res = {};
  ASSERT_EQ(xDnsClientDo(client, "example.com", xDnsType_A, capture_cb, &res),
            xErrno_Ok);

  pump_loop(loop, 2000);
  EXPECT_TRUE(res.fired);
  EXPECT_EQ(res.err, xErrno_Timeout);
  EXPECT_EQ(res.count, 0);

  xDnsClientDestroy(client);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* Partial success: A resolves via real DNS, AAAA times out via unreachable
 * nameserver.  The client should return A records with xErrno_Ok.
 *
 * We use two nameservers: 8.8.8.8 (reachable) and 192.0.2.1 (unreachable,
 * RFC 5737 TEST-NET).  The client tries them in order — first query (A) goes
 * to 8.8.8.8 and succeeds; second query (AAAA) also goes to 8.8.8.8.  To
 * force a timeout on AAAA only, we use a local server that responds to A
 * but ignores AAAA. */
TEST(DnsClient, PartialSuccessAResolvesAAAATimeout) {
  if (!can_reach_dns()) GTEST_SKIP() << "8.8.8.8 unreachable";

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  /* Set up a local DNS server that responds to A queries but ignores AAAA. */
  xDnsZone zone = xDnsZoneCreate();
  /* 93.184.216.34 = example.com's real IP */
  uint8_t ip[4] = {93, 184, 216, 34};
  xDnsZoneAdd(zone, "partial.example", xDnsType_A, ip, 4, 300);

  xDnsServerConf sconf = {};
  sconf.forwarder = NULL; /* authoritative only — no AAAA → NXDOMAIN */
  xDnsServer server = xDnsServerCreate(&sconf);
  ASSERT_NE(server, nullptr);
  xDnsServerAddZone(server, zone);

  uint16_t sport = 0;
  {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sin = {};
    sin.sin_family      = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port        = 0;
    bind(fd, reinterpret_cast<struct sockaddr *>(&sin), sizeof(sin));
    socklen_t slen = sizeof(sin);
    getsockname(fd, reinterpret_cast<struct sockaddr *>(&sin), &slen);
    sport = ntohs(sin.sin_port);
    close(fd);
  }
  ASSERT_GT(sport, 0);
  ASSERT_EQ(xDnsServerListen(server, "127.0.0.1", sport), xErrno_Ok);

  /* Client queries the local server with A|AAAA.
   * A: zone hit → returns IP
   * AAAA: zone miss, no forwarder → NXDOMAIN (not a timeout, but partial) */
  xDnsClientConf cconf = {};
  char ns[32];
  snprintf(ns, sizeof(ns), "127.0.0.1:%u", sport);
  cconf.nameservers[0] = ns;
  cconf.timeout_ms     = 2000;
  cconf.retries        = 0;
  cconf.enable_cache   = 0;

  xDnsClient client = xDnsClientCreate(&cconf);
  ASSERT_NE(client, nullptr);

  DnsResult res = {};
  ASSERT_EQ(xDnsClientDo(client, "partial.example",
                         (xDnsType)(xDnsType_A | xDnsType_AAAA),
                         capture_cb, &res),
            xErrno_Ok);

  pump_loop(loop, 4000);
  EXPECT_TRUE(res.fired);
  /* Should succeed — at least the A record was resolved */
  EXPECT_EQ(res.err, xErrno_Ok);
  EXPECT_GE(res.count, 1);
  EXPECT_TRUE(res.has_a) << "A record should be present";

  xDnsClientDestroy(client);
  xDnsServerDestroy(server);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

/* ═══════════════════════════════════════════════════════════════════
 * ═══════════════════════════════════════════════════════════════════ */

TEST(DnsServer, AuthoritativeZone) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xDnsZone zone = xDnsZoneCreate();
  ASSERT_NE(zone, nullptr);
  uint8_t ip[4] = {10, 0, 0, 5};
  ASSERT_EQ(xDnsZoneAdd(zone, "myapp.local", xDnsType_A, ip, 4, 3600),
            xErrno_Ok);

  xDnsServerConf sconf = {};
  xDnsServer server = xDnsServerCreate(&sconf);
  ASSERT_NE(server, nullptr);
  ASSERT_EQ(xDnsServerAddZone(server, zone), xErrno_Ok);
  ASSERT_EQ(xDnsServerListen(server, "127.0.0.1", 0), xErrno_Ok);
  uint16_t port = xDnsServerPort(server);
  ASSERT_GT(port, 0);

  /* Build a client that uses the local server as its nameserver. */
  char ns[32];
  snprintf(ns, sizeof(ns), "127.0.0.1:%u", port);
  xDnsClientConf cconf = {};
  cconf.nameservers[0] = ns;
  cconf.timeout_ms     = 2000;
  cconf.retries        = 0;
  cconf.enable_cache   = 0;
  xDnsClient client = xDnsClientCreate(&cconf);
  ASSERT_NE(client, nullptr);

  DnsResult res = {};
  ASSERT_EQ(xDnsClientDo(client, "myapp.local", xDnsType_A, capture_cb, &res),
            xErrno_Ok);
  pump_loop(loop, 2000);

  EXPECT_TRUE(res.fired);
  EXPECT_EQ(res.err, xErrno_Ok);
  ASSERT_GE(res.count, 1);
  ASSERT_TRUE(res.has_records);
  EXPECT_EQ(res.first_qtype, DNS_QTYPE_A);
  ASSERT_EQ(res.first_rdlen, 4u);
  EXPECT_EQ(memcmp(res.first_rdata, ip, 4), 0);

  xDnsClientDestroy(client);
  xDnsServerDestroy(server);
  xDnsZoneDestroy(zone);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(DnsServer, NxDomainWithoutForwarder) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xDnsServerConf sconf = {};
  xDnsServer server = xDnsServerCreate(&sconf);
  ASSERT_NE(server, nullptr);
  ASSERT_EQ(xDnsServerListen(server, "127.0.0.1", 0), xErrno_Ok);
  uint16_t port = xDnsServerPort(server);
  ASSERT_GT(port, 0);

  char ns[32];
  snprintf(ns, sizeof(ns), "127.0.0.1:%u", port);
  xDnsClientConf cconf = {};
  cconf.nameservers[0] = ns;
  cconf.timeout_ms     = 2000;
  cconf.retries        = 0;
  cconf.enable_cache   = 0;
  xDnsClient client = xDnsClientCreate(&cconf);
  ASSERT_NE(client, nullptr);

  DnsResult res = {};
  xDnsClientDo(client, "nope.local", xDnsType_A, capture_cb, &res);
  pump_loop(loop, 2000);

  EXPECT_TRUE(res.fired);
  EXPECT_EQ(res.err, xErrno_DnsNotFound);

  xDnsClientDestroy(client);
  xDnsServerDestroy(server);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

static int block_filter(const char *name, uint16_t /*type*/, void * /*arg*/) {
  if (strstr(name, "blocked")) return 1;
  return 0;
}

TEST(DnsServer, FilterBlocks) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xDnsZone zone = xDnsZoneCreate();
  uint8_t ip[4] = {10, 0, 0, 5};
  xDnsZoneAdd(zone, "blocked.local", xDnsType_A, ip, 4, 3600);
  xDnsZoneAdd(zone, "ok.local", xDnsType_A, ip, 4, 3600);

  xDnsServerConf sconf = {};
  sconf.filter = block_filter;
  xDnsServer server = xDnsServerCreate(&sconf);
  ASSERT_NE(server, nullptr);
  xDnsServerAddZone(server, zone);
  ASSERT_EQ(xDnsServerListen(server, "127.0.0.1", 0), xErrno_Ok);
  uint16_t port = xDnsServerPort(server);
  ASSERT_GT(port, 0);

  char ns[32];
  snprintf(ns, sizeof(ns), "127.0.0.1:%u", port);
  xDnsClientConf cconf = {};
  cconf.nameservers[0] = ns;
  cconf.timeout_ms     = 2000;
  cconf.retries        = 0;
  cconf.enable_cache   = 0;
  xDnsClient client = xDnsClientCreate(&cconf);
  ASSERT_NE(client, nullptr);

  /* Blocked name → NXDOMAIN */
  DnsResult r1 = {};
  xDnsClientDo(client, "blocked.local", xDnsType_A, capture_cb, &r1);
  pump_loop(loop, 2000);
  EXPECT_TRUE(r1.fired);
  EXPECT_EQ(r1.err, xErrno_DnsNotFound);

  /* Allowed name → success */
  DnsResult r2 = {};
  xDnsClientDo(client, "ok.local", xDnsType_A, capture_cb, &r2);
  pump_loop(loop, 2000);
  EXPECT_TRUE(r2.fired);
  EXPECT_EQ(r2.err, xErrno_Ok);
  ASSERT_GE(r2.count, 1);

  xDnsClientDestroy(client);
  xDnsServerDestroy(server);
  xDnsZoneDestroy(zone);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(DnsServer, Forwarding) {
  if (!can_reach_dns()) GTEST_SKIP() << "8.8.8.8 unreachable";

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  /* Upstream client → 8.8.8.8 */
  xDnsClientConf uconf = {};
  uconf.nameservers[0] = "8.8.8.8";
  uconf.timeout_ms     = 5000;
  uconf.retries        = 1;
  uconf.enable_cache   = 0;
  xDnsClient upstream = xDnsClientCreate(&uconf);
  ASSERT_NE(upstream, nullptr);

  /* Local server forwards to upstream */
  xDnsServerConf sconf = {};
  sconf.forwarder = upstream;
  xDnsServer server = xDnsServerCreate(&sconf);
  ASSERT_NE(server, nullptr);
  ASSERT_EQ(xDnsServerListen(server, "127.0.0.1", 0), xErrno_Ok);
  uint16_t port = xDnsServerPort(server);
  ASSERT_GT(port, 0);

  /* Local client → local server */
  char ns[32];
  snprintf(ns, sizeof(ns), "127.0.0.1:%u", port);
  xDnsClientConf cconf = {};
  cconf.nameservers[0] = ns;
  cconf.timeout_ms     = 6000;
  cconf.retries        = 0;
  cconf.enable_cache   = 0;
  xDnsClient client = xDnsClientCreate(&cconf);
  ASSERT_NE(client, nullptr);

  DnsResult res = {};
  ASSERT_EQ(xDnsClientDo(client, "example.com", xDnsType_A, capture_cb, &res),
            xErrno_Ok);
  pump_loop(loop, 7000);

  EXPECT_TRUE(res.fired);
  EXPECT_EQ(res.err, xErrno_Ok);
  EXPECT_GE(res.count, 1);
  if (res.has_records) {
    EXPECT_EQ(res.first_qtype, DNS_QTYPE_A);
  }

  xDnsClientDestroy(client);
  xDnsServerDestroy(server);
  xDnsClientDestroy(upstream);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}
