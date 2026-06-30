/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns_server.c - DNS server with authoritative zones + forwarding
 */

#include "dns_private.h"

#include <stdlib.h>
#include <string.h>

#include <x/base/socket.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

/* ───────────────────── Zone ───────────────────── */

typedef struct zone_rec {
  char             name[256];
  uint16_t         qtype; /* wire QTYPE                              */
  uint32_t         ttl;
  void            *rdata; /* owned                                   */
  size_t           rdlen;
  struct zone_rec *next;
} zone_rec_t;

struct xDnsZone_ {
  zone_rec_t *head;
};

xDnsZone xDnsZoneCreate(void) {
  return (xDnsZone)calloc(1, sizeof(struct xDnsZone_));
}

void xDnsZoneDestroy(xDnsZone zone) {
  if (!zone) return;
  struct xDnsZone_ *z = (struct xDnsZone_ *)zone;
  zone_rec_t       *r = z->head;
  while (r) {
    zone_rec_t *next = r->next;
    free(r->rdata);
    free(r);
    r = next;
  }
  free(z);
}

xErrno xDnsZoneAdd(xDnsZone zone, const char *name, xDnsType type, const void *rdata, size_t rdlen,
                   uint32_t ttl) {
  if (!zone || !name || type == 0) return xErrno_InvalidArg;
  struct xDnsZone_ *z = (struct xDnsZone_ *)zone;

  uint16_t qt = dns_qtype_from_bit(type);
  if (qt == 0) return xErrno_InvalidArg;

  zone_rec_t *r = (zone_rec_t *)calloc(1, sizeof(zone_rec_t));
  if (!r) return xErrno_NoMemory;
  strncpy(r->name, name, sizeof(r->name) - 1);
  r->name[sizeof(r->name) - 1] = '\0';
  /* lowercase */
  for (char *p = r->name; *p; ++p)
    if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
  r->qtype = qt;
  r->ttl   = ttl;
  r->rdlen = rdlen;
  if (rdlen > 0) {
    r->rdata = malloc(rdlen);
    if (!r->rdata) {
      free(r);
      return xErrno_NoMemory;
    }
    memcpy(r->rdata, rdata, rdlen);
  }
  r->next = z->head;
  z->head = r;
  return xErrno_Ok;
}

/* Find matching records in a zone. Returns a cloned xDnsRecord list
 * (caller frees) or NULL. */
static xDnsRecord *zone_lookup(struct xDnsZone_ *z, const char *name, uint16_t qtype) {
  if (!z) return NULL;
  xDnsRecord *head = NULL, *tail = NULL;
  for (zone_rec_t *r = z->head; r; r = r->next) {
    if (r->qtype != qtype) continue;
    if (strcmp(r->name, name) != 0) continue;
    xDnsRecord *rec = (xDnsRecord *)calloc(1, sizeof(xDnsRecord));
    if (!rec) goto fail;
    rec->qtype    = r->qtype;
    rec->ttl      = r->ttl;
    rec->rdlength = r->rdlen;
    {
      size_t nl = strlen(r->name);
      char  *nm = (char *)malloc(nl + 1);
      if (!nm) {
        free(rec);
        goto fail;
      }
      memcpy(nm, r->name, nl + 1);
      rec->name = nm;
    }
    if (r->rdlen > 0) {
      void *rd = malloc(r->rdlen);
      if (!rd) {
        free((void *)rec->name);
        free(rec);
        goto fail;
      }
      memcpy(rd, r->rdata, r->rdlen);
      rec->rdata = rd;
    }
    if (!head)
      head = rec;
    else
      tail->next = rec;
    tail = rec;
  }
  return head;
fail:
  dns_records_free(head);
  return NULL;
}

/* ───────────────────── Server ───────────────────── */

struct xDnsServer_ {
  xSocket            sock;
  int                sock_family;
  xDnsClient         forwarder;
  xDnsFilterFunc     filter;
  void              *filter_arg;
  int                cache_enabled;
  uint16_t           port;
  struct xDnsZone_ **zones;
  int                zone_count;
  int                zone_cap;
};

/* Context for a forwarded query. */
typedef struct {
  struct xDnsServer_     *server;
  struct sockaddr_storage client_addr;
  socklen_t               client_len;
  uint16_t                id;
  char                    name[256];
  uint16_t                qtype;
} fwd_ctx_t;

/* ───────────────────── Forward declarations ───────────────────── */

static void     on_server_readable(xSocket sock, xEventMask mask, void *arg);
static void     on_forward_done(xErrno err, const xDnsRecord *records, void *arg);
static xDnsType qtype_to_bit(uint16_t qtype);

/* ───────────────────── Lifecycle ───────────────────── */

xDnsServer xDnsServerCreate(const xDnsServerConf *conf) {
  struct xDnsServer_ *s = (struct xDnsServer_ *)calloc(1, sizeof(*s));
  if (!s) return NULL;
  if (conf) {
    s->forwarder     = conf->forwarder;
    s->filter        = conf->filter;
    s->filter_arg    = conf->filter_arg;
    s->cache_enabled = conf->cache_enabled ? 1 : 0;
  }
  return (xDnsServer)s;
}

void xDnsServerDestroy(xDnsServer server) {
  if (!server) return;
  struct xDnsServer_ *s = (struct xDnsServer_ *)server;
  /* Zones are NOT freed here — caller owns them. */
  free(s->zones);
  if (s->sock) xSocketDestroy(s->sock);
  free(s);
}

xErrno xDnsServerAddZone(xDnsServer server, xDnsZone zone) {
  if (!server || !zone) return xErrno_InvalidArg;
  struct xDnsServer_ *s = (struct xDnsServer_ *)server;
  if (s->zone_count == s->zone_cap) {
    int                ncap = s->zone_cap ? s->zone_cap * 2 : 4;
    struct xDnsZone_ **nz   = (struct xDnsZone_ **)realloc(s->zones, ncap * sizeof(*nz));
    if (!nz) return xErrno_NoMemory;
    s->zones    = nz;
    s->zone_cap = ncap;
  }
  s->zones[s->zone_count++] = (struct xDnsZone_ *)zone;
  return xErrno_Ok;
}

static xErrno bind_socket(struct xDnsServer_ *s, const char *host, uint16_t port) {
  /* Try IPv6 dual-stack first. */
  xSocket sock = xSocketCreate(AF_INET6, SOCK_DGRAM, 0, xEvent_Read, on_server_readable, s);
  if (sock) {
    int fd     = xSocketFd(sock);
    int v6only = 0;
#ifdef _WIN32
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof(v6only));
#else
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
    struct sockaddr_in6 sin6;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port   = htons(port);
    if (host && *host) {
      if (inet_pton(AF_INET6, host, &sin6.sin6_addr) != 1) {
        xSocketDestroy(sock);
        sock = NULL;
      }
    } /* else ::0 (any) */
    if (sock) {
      if (bind(fd, (struct sockaddr *)&sin6, sizeof(sin6)) != 0) {
        xSocketDestroy(sock);
        sock = NULL;
      }
    }
    if (sock) {
      s->sock        = sock;
      s->sock_family = AF_INET6;
      goto bound;
    }
  }

  /* Fall back to IPv4. */
  sock = xSocketCreate(AF_INET, SOCK_DGRAM, 0, xEvent_Read, on_server_readable, s);
  if (!sock) return xErrno_SysError;
  int                fd = xSocketFd(sock);
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port   = htons(port);
  if (host && *host) {
    if (inet_pton(AF_INET, host, &sin.sin_addr) != 1) {
      xSocketDestroy(sock);
      return xErrno_InvalidArg;
    }
  } else {
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
  }
  if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
    xSocketDestroy(sock);
    return xErrno_SysError;
  }
  s->sock        = sock;
  s->sock_family = AF_INET;

bound:
  /* Discover actual port (for ephemeral). */
  if (port == 0) {
    struct sockaddr_storage ss;
    socklen_t               sslen = sizeof(ss);
    if (getsockname(xSocketFd(s->sock), (struct sockaddr *)&ss, &sslen) == 0) {
      if (ss.ss_family == AF_INET)
        s->port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
      else
        s->port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    }
  } else {
    s->port = port;
  }
  return xErrno_Ok;
}

xErrno xDnsServerListen(xDnsServer server, const char *host, uint16_t port) {
  if (!server) return xErrno_InvalidArg;
  struct xDnsServer_ *s = (struct xDnsServer_ *)server;
  if (s->sock) return xErrno_AlreadyExists;
  return bind_socket(s, host, port);
}

uint16_t xDnsServerPort(xDnsServer server) {
  if (!server) return 0;
  return ((struct xDnsServer_ *)server)->port;
}

/* ───────────────────── Query handling ───────────────────── */

static void send_response(struct xDnsServer_ *s, const struct sockaddr *addr, socklen_t addrlen,
                          uint16_t id, int rcode, const char *qname, uint16_t qtype,
                          const xDnsRecord *answers) {
  uint8_t buf[DNS_EDNS0_SIZE];
  int     n = dns_build_response(buf, sizeof(buf), id, rcode, qname, qtype, answers);
  if (n <= 0) return;
  xSocketSendTo(s->sock, buf, (size_t)n, addr, addrlen);
}

static xDnsType qtype_to_bit(uint16_t qtype) {
  switch (qtype) {
  case DNS_QTYPE_A:
    return xDnsType_A;
  case DNS_QTYPE_AAAA:
    return xDnsType_AAAA;
  case DNS_QTYPE_CNAME:
    return xDnsType_CNAME;
  default:
    return (xDnsType)0;
  }
}

static void handle_query(struct xDnsServer_ *s, const uint8_t *buf, size_t len,
                         const struct sockaddr *addr, socklen_t addrlen) {
  dns_header_t   hdr;
  dns_question_t q;
  xDnsRecord    *answers = NULL;
  if (dns_parse(buf, len, &hdr, &q, &answers) != xErrno_Ok) return;

  /* Only handle standard queries (QR=0, opcode=0). */
  if ((hdr.flags & DNS_FLAG_QR) != 0) {
    dns_records_free(answers);
    return;
  }

  /* lowercase the query name for matching */
  for (char *p = q.name; *p; ++p)
    if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');

  /* Filter */
  if (s->filter && s->filter(q.name, q.qtype, s->filter_arg) != 0) {
    send_response(s, addr, addrlen, hdr.id, (int)DNS_RCODE_NXDOMAIN, q.name, q.qtype, NULL);
    dns_records_free(answers);
    return;
  }

  /* Zone lookup */
  for (int i = 0; i < s->zone_count; ++i) {
    xDnsRecord *zr = zone_lookup(s->zones[i], q.name, q.qtype);
    if (zr) {
      send_response(s, addr, addrlen, hdr.id, 0, q.name, q.qtype, zr);
      dns_records_free(zr);
      dns_records_free(answers);
      return;
    }
  }

  /* Forwarding */
  if (s->forwarder) {
    xDnsType bit = qtype_to_bit(q.qtype);
    if (bit == 0) {
      send_response(s, addr, addrlen, hdr.id, (int)DNS_RCODE_NXDOMAIN, q.name, q.qtype, NULL);
      dns_records_free(answers);
      return;
    }
    fwd_ctx_t *ctx = (fwd_ctx_t *)calloc(1, sizeof(fwd_ctx_t));
    if (!ctx) {
      dns_records_free(answers);
      return;
    }
    ctx->server = s;
    memcpy(&ctx->client_addr, addr, addrlen);
    ctx->client_len = addrlen;
    ctx->id         = hdr.id;
    ctx->qtype      = q.qtype;
    strncpy(ctx->name, q.name, sizeof(ctx->name) - 1);
    ctx->name[sizeof(ctx->name) - 1] = '\0';
    dns_records_free(answers);
    xErrno e = xDnsClientDo(s->forwarder, ctx->name, bit, on_forward_done, ctx);
    if (e != xErrno_Ok) {
      send_response(s, addr, addrlen, hdr.id, 2 /* SERVFAIL */, ctx->name, ctx->qtype, NULL);
      free(ctx);
    }
    return;
  }

  /* No forwarder → NXDOMAIN */
  send_response(s, addr, addrlen, hdr.id, (int)DNS_RCODE_NXDOMAIN, q.name, q.qtype, NULL);
  dns_records_free(answers);
}

static void on_forward_done(xErrno err, const xDnsRecord *records, void *arg) {
  fwd_ctx_t *ctx = (fwd_ctx_t *)arg;
  int        rcode;
  if (err == xErrno_Ok)
    rcode = 0;
  else if (err == xErrno_DnsNotFound)
    rcode = (int)DNS_RCODE_NXDOMAIN;
  else
    rcode = 2; /* SERVFAIL */
  send_response(ctx->server, (const struct sockaddr *)&ctx->client_addr, ctx->client_len, ctx->id,
                rcode, ctx->name, ctx->qtype, (err == xErrno_Ok) ? records : NULL);
  free(ctx);
}

/* ───────────────────── Readable callback ───────────────────── */

static void on_server_readable(xSocket sock, xEventMask mask, void *arg) {
  (void)sock;
  if (!(mask & xEvent_Read)) return;
  struct xDnsServer_ *s = (struct xDnsServer_ *)arg;

  uint8_t                 buf[DNS_EDNS0_SIZE + 1];
  struct sockaddr_storage src;
  socklen_t               srclen;
  for (;;) {
    srclen    = sizeof(src);
    ssize_t n = xSocketRecvFrom(s->sock, buf, sizeof(buf), (struct sockaddr *)&src, &srclen);
    if (n < 0) break;
    handle_query(s, (const uint8_t *)buf, (size_t)n, (const struct sockaddr *)&src, srclen);
  }
}
