/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns_client.c - Async DNS resolver over UDP
 *
 * Single non-blocking UDP socket multiplexes concurrent queries by 16-bit
 * transaction ID. A multi-type request (e.g. A|AAAA) sends one UDP query
 * per bit and merges the results before invoking the callback once.
 */

#include "dns_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <x/base/map.h>
#include <x/base/socket.h>
#include <x/base/time.h>

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

/* ───────────────────── Types ───────────────────── */

typedef struct query query_t;
typedef struct request request_t;

/** Per-nameserver UDP connection. */
typedef struct {
  xSocket                 sock;         /* UDP socket, or NULL if not open */
  struct sockaddr_storage addr;         /* nameserver address              */
  socklen_t               addrlen;      /* length of addr                  */
  int                     query_count;  /* sent since open                 */
  int                     max_queries;  /* rotation threshold (0=unlim)    */
} conn_t;

struct query {
  uint16_t    id;
  uint16_t    qtype;       /* wire QTYPE                                */
  char        name[256];
  int         ns_index;    /* current nameserver index                  */
  int         retries_left;
  xTimer      timer;
  query_t    *next;        /* sibling queries within the same request   */
  request_t  *req;
};

struct request {
  xDnsClient   client;
  xDnsCallback cb;
  void        *arg;
  int          pending;    /* outstanding queries                       */
  xDnsRecord  *results;    /* accumulated (append at tail)              */
  xDnsRecord  *results_tail;
  int          had_success;
  xErrno       last_err;
  query_t     *queries;    /* head of query list                        */
};

struct xDnsClient_ {
  conn_t     conns[8];             /* per-nameserver connections          */
  int        sock_family;          /* AF_INET or AF_INET6               */
  xMap       queries;              /* id (void*) → query_t*              */
  xMap       cache;                /* NULL if disabled                   */
  xMap       hosts;                /* NULL if disabled                   */
  /* Nameserver addresses (pre-resolved) */
  struct sockaddr_storage ns_addr[8];
  socklen_t               ns_len[8];
  int                     ns_count;
  int      timeout_ms;
  int      retries;
  int      enable_cache;
  int      enable_hosts;
  uint16_t next_id;
};

/* ───────────────────── Forward declarations ───────────────────── */

static void on_readable(xSocket sock, xEventMask mask, void *arg);
static void on_query_timeout(void *arg);
static void query_destroy(query_t *q);
static void request_maybe_complete(request_t *req);
static xErrno send_query(struct xDnsClient_ *c, query_t *q);

/* ───────────────────── Helpers ───────────────────── */

static void id_set(xMap m, uint16_t id, query_t *q) {
  xMapSet(m, (void *)(uintptr_t)(id + 1), q);
}
static query_t *id_get(xMap m, uint16_t id) {
  return (query_t *)xMapGet(m, (void *)(uintptr_t)(id + 1));
}
static query_t *id_del(xMap m, uint16_t id) {
  return (query_t *)xMapDel(m, (void *)(uintptr_t)(id + 1));
}

static uint16_t alloc_id(struct xDnsClient_ *c) {
  for (int i = 0; i < 65536; ++i) {
    uint16_t id = c->next_id++;
    if (id == 0) continue; /* 0 is reserved as the "exhausted" sentinel */
    if (!id_get(c->queries, id)) return id;
  }
  return 0; /* exhausted (extremely unlikely) */
}

/* ───────────────────── Connection management ───────────────────── */

/** Open a UDP socket for conn and register with the event loop. */
static int conn_open(struct xDnsClient_ *c, conn_t *conn) {
  xSocket s = xSocketCreate(c->sock_family, SOCK_DGRAM, 0, xEvent_Read,
                            on_readable, c);
  if (!s) return -1;
  int fd = xSocketFd(s);
  if (c->sock_family == AF_INET6) {
    int v6only = 0;
#ifdef _WIN32
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof(v6only));
#else
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
  }
  conn->sock        = s;
  conn->query_count = 0;
  return 0;
}

/** Close an existing connection and free its socket. */
static void conn_close(conn_t *conn) {
  if (conn->sock) {
    xSocketDestroy(conn->sock);
    conn->sock = NULL;
  }
  conn->query_count = 0;
}

/** Rotate a connection: close old socket, open new with fresh source port. */
static int conn_rotate(struct xDnsClient_ *c, conn_t *conn) {
  conn_close(conn);
  return conn_open(c, conn);
}

/** Initialize all connections with nameserver addresses. */
static int conns_init(struct xDnsClient_ *c) {
  /* Determine socket family */
  int need_v6 = 0;
  for (int i = 0; i < c->ns_count; ++i)
    if (c->ns_addr[i].ss_family == AF_INET6) { need_v6 = 1; break; }
  c->sock_family = need_v6 ? AF_INET6 : AF_INET;

  for (int i = 0; i < c->ns_count; ++i) {
    memcpy(&c->conns[i].addr, &c->ns_addr[i], sizeof(c->ns_addr[i]));
    c->conns[i].addrlen    = c->ns_len[i];
    c->conns[i].max_queries = 0; /* set below from config */
    if (conn_open(c, &c->conns[i]) != 0) return -1;
  }
  return 0;
}

/** Close all connections. */
static void conns_cleanup(struct xDnsClient_ *c) {
  for (int i = 0; i < c->ns_count; ++i) conn_close(&c->conns[i]);
}

/** Find the connection belonging to a given socket fd. */
static conn_t *conn_find(struct xDnsClient_ *c, xSocket sock) {
  for (int i = 0; i < c->ns_count; ++i)
    if (c->conns[i].sock == sock) return &c->conns[i];
  return NULL;
}

/** Ensure a connection is ready: open if needed, rotate if over limit. */
static conn_t *conn_ready(struct xDnsClient_ *c, int ns_index) {
  if (ns_index < 0 || ns_index >= c->ns_count) return NULL;
  conn_t *conn = &c->conns[ns_index];
  if (!conn->sock && conn_open(c, conn) != 0) return NULL;
  if (conn->max_queries > 0 && conn->query_count >= conn->max_queries) {
    if (conn_rotate(c, conn) != 0) return NULL;
  }
  return conn;
}

/* Resolve the nameserver address for sending. If the socket is AF_INET6
 * but the nameserver is IPv4, produce an IPv4-mapped IPv6 address. */
static const struct sockaddr *resolve_ns(struct xDnsClient_ *c, int idx,
                                         struct sockaddr_in6 *mapped,
                                         socklen_t *outlen) {
  struct sockaddr_storage *ss = &c->ns_addr[idx];
  if (c->sock_family == AF_INET6 && ss->ss_family == AF_INET) {
    struct sockaddr_in *in = (struct sockaddr_in *)ss;
    memset(mapped, 0, sizeof(*mapped));
    mapped->sin6_family   = AF_INET6;
    mapped->sin6_port     = in->sin_port;
    ((uint8_t *)&mapped->sin6_addr)[10] = 0xff;
    ((uint8_t *)&mapped->sin6_addr)[11] = 0xff;
    memcpy(&mapped->sin6_addr.s6_addr[12], &in->sin_addr, 4);
    *outlen = sizeof(*mapped);
    return (const struct sockaddr *)mapped;
  }
  *outlen = c->ns_len[idx];
  return (const struct sockaddr *)ss;
}

/* ───────────────────── Hosts file ───────────────────── */

static xMap hosts_load(void) {
  FILE *fp;
#ifdef _WIN32
  fp = fopen("C:\\Windows\\System32\\drivers\\etc\\hosts", "r");
#else
  fp = fopen("/etc/hosts", "r");
#endif
  if (!fp) return NULL;

  xMap m = xMapCreate(xMapType_Hash, 64, xMapStrHash, xMapStrEq);
  if (!m) { fclose(fp); return NULL; }

  char line[1024];
  while (fgets(line, sizeof(line), fp)) {
    /* Strip comment */
    char *c = strchr(line, '#');
    if (c) *c = '\0';

    /* Parse: IP hostname [alias...] */
    char ip[64], name[256];
    int n = 0;
    if (sscanf(line, "%63s %255s%n", ip, name, &n) < 2) continue;
    if (ip[0] == '\0' || name[0] == '\0') continue;

    /* Only support IPv4 for now */
    struct sockaddr_in sin;
    if (inet_pton(AF_INET, ip, &sin.sin_addr) != 1) continue;

    /* Build xDnsRecord */
    xDnsRecord *rec = (xDnsRecord *)calloc(1, sizeof(xDnsRecord));
    if (!rec) continue;
    rec->qtype = DNS_QTYPE_A;
    rec->ttl   = 0; /* hosts entries never expire */
    rec->name  = strdup(name);
    rec->rdata = malloc(4);
    if (!rec->name || !rec->rdata) {
      free((void *)rec->name); free((void *)rec->rdata); free(rec); continue;
    }
    memcpy((void *)rec->rdata, &sin.sin_addr, 4);
    rec->rdlength = 4;

    /* Insert — support multiple IPs per hostname via linked list */
    xDnsRecord *existing = (xDnsRecord *)xMapGet(m, name);
    if (existing) {
      xDnsRecord *tail = existing;
      while (tail->next) tail = tail->next;
      tail->next = rec;
    } else {
      xMapSet(m, name, rec);
    }

    /* Also add alias entries. e.g. "127.0.0.1 localhost myapp.local" */
    char *alias = line + n;
    while (*alias) {
      char aname[256];
      if (sscanf(alias, "%255s%n", aname, &n) != 1) break;
      if (aname[0] == '\0') break;
      if (strcasecmp(aname, name) == 0) { alias += n; continue; }
      /* Create a duplicate record for the alias */
      xDnsRecord *arec = (xDnsRecord *)calloc(1, sizeof(xDnsRecord));
      if (!arec) break;
      arec->qtype    = DNS_QTYPE_A;
      arec->ttl      = 0;
      arec->name     = strdup(aname);
      arec->rdata    = malloc(4);
      if (!arec->name || !arec->rdata) {
        free((void *)arec->name); free((void *)arec->rdata); free(arec); break;
      }
      memcpy((void *)arec->rdata, &sin.sin_addr, 4);
      arec->rdlength = 4;
      xDnsRecord *aexist = (xDnsRecord *)xMapGet(m, aname);
      if (aexist) {
        xDnsRecord *atail = aexist;
        while (atail->next) atail = atail->next;
        atail->next = arec;
      } else {
        xMapSet(m, aname, arec);
      }
      alias += n;
    }
  }
  fclose(fp);
  return m;
}

static xDnsRecord *hosts_lookup(xMap m, const char *name, uint16_t qtype) {
  if (!m || !name) return NULL;
  xDnsRecord *rec = (xDnsRecord *)xMapGet(m, name);
  if (!rec) return NULL;
  /* Filter by qtype */
  for (; rec; rec = rec->next) if (rec->qtype == qtype) break;
  return rec;
}

static void hosts_record_free(xDnsRecord *rec) {
  xDnsRecord *next;
  for (; rec; rec = next) {
    next = rec->next;
    free((void *)rec->name);
    free((void *)rec->rdata);
    free(rec);
  }
}

static bool hosts_free_cb(const void *key, void *val, void *arg) {
  (void)key; (void)arg;
  hosts_record_free((xDnsRecord *)val);
  return true;
}

static void hosts_destroy(xMap m) {
  if (!m) return;
  xMapIterate(m, hosts_free_cb, NULL);
  xMapDestroy(m);
}

/* ───────────────────── Lifecycle ───────────────────── */

xDnsClient xDnsClientCreate(const xDnsClientConf *conf) {
  struct xDnsClient_ *c = (struct xDnsClient_ *)calloc(1, sizeof(*c));
  if (!c) return NULL;

  c->queries = xMapCreate(xMapType_Hash, 64, xMapIntHash, xMapIntEq);
  if (!c->queries) {
    free(c);
    return NULL;
  }

  c->timeout_ms   = 5000;
  c->retries      = 2;
  c->enable_cache = 1;
  c->enable_hosts = 1;

  /* Nameservers from config or system discovery. */
  char nss[8][46];
  int  ns_count = 0;
  if (conf) {
    c->timeout_ms   = conf->timeout_ms   > 0 ? conf->timeout_ms : 5000;
    c->retries      = conf->retries      >= 0 ? conf->retries    : 2;
    c->enable_cache = conf->enable_cache    ? 1 : 0;
    c->enable_hosts = conf->enable_hosts    ? 1 : 0;
    for (int i = 0; i < 8 && conf->nameservers[i]; ++i) {
      strncpy(nss[i], conf->nameservers[i], 45);
      nss[i][45] = '\0';
      ns_count = i + 1;
    }
  }
  if (ns_count == 0) {
    ns_count = dns_config_load_nameservers(nss, 8);
  }

  /* Resolve nameserver strings into sockaddr_storage.
   * Each string may be "host" (port 53), "ipv4:port", or "[ipv6]:port". */
  for (int i = 0; i < ns_count && c->ns_count < 8; ++i) {
    const char *s = nss[i];
    char host[64];
    int  port = DNS_PORT;

    if (s[0] == '[') {
      /* [ipv6]:port */
      const char *end = strchr(s, ']');
      if (!end) continue;
      size_t hl = (size_t)(end - s - 1);
      if (hl >= sizeof(host)) continue;
      memcpy(host, s + 1, hl);
      host[hl] = '\0';
      if (end[1] == ':') port = atoi(end + 2);
    } else {
      const char *c1 = strchr(s, ':');
      const char *c2 = c1 ? strrchr(s, ':') : NULL;
      if (c1 && c1 == c2) {
        /* single colon → ipv4:port */
        size_t hl = (size_t)(c1 - s);
        if (hl >= sizeof(host)) continue;
        memcpy(host, s, hl);
        host[hl] = '\0';
        port = atoi(c1 + 1);
      } else {
        /* no colon (ipv4 or ipv6) or multiple colons (bare ipv6) */
        strncpy(host, s, sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
      }
    }
    if (port <= 0) port = DNS_PORT;

    /* Try IPv4 first. */
    struct sockaddr_in  sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sin.sin_addr) == 1) {
      memcpy(&c->ns_addr[c->ns_count], &sin, sizeof(sin));
      c->ns_len[c->ns_count] = sizeof(sin);
      ++c->ns_count;
      continue;
    }
    /* Then IPv6. */
    struct sockaddr_in6 sin6;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET6, host, &sin6.sin6_addr) == 1) {
      memcpy(&c->ns_addr[c->ns_count], &sin6, sizeof(sin6));
      c->ns_len[c->ns_count] = sizeof(sin6);
      ++c->ns_count;
    }
  }
  if (c->ns_count == 0) {
    /* absolute fallback */
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(DNS_PORT);
    inet_pton(AF_INET, "8.8.8.8", &sin.sin_addr);
    memcpy(&c->ns_addr[0], &sin, sizeof(sin));
    c->ns_len[0]  = sizeof(sin);
    c->ns_count   = 1;
  }

  if (c->enable_cache) {
    c->cache = dns_cache_create();
    if (!c->cache) {
      xMapDestroy(c->queries);
      free(c);
      return NULL;
    }
  }

  /* Initialize per-nameserver connections. */
  if (conns_init(c) != 0) {
    if (c->cache) dns_cache_destroy(c->cache);
    xMapDestroy(c->queries);
    free(c);
    return NULL;
  }

  /* Apply per-connection max_queries from config. */
  int max_q = conf ? conf->udp_max_queries : 0;
  for (int i = 0; i < c->ns_count; ++i) c->conns[i].max_queries = max_q;

  /* Load hosts file if enabled. */
  if (c->enable_hosts) c->hosts = hosts_load();

  return (xDnsClient)c;
}

void xDnsClientDestroy(xDnsClient client) {
  if (!client) return;
  struct xDnsClient_ *c = (struct xDnsClient_ *)client;

  /* Cancel all outstanding queries (callbacks NOT invoked). Collect unique
   * requests so each is freed exactly once. */
  if (c->queries) {
    xMap          m = c->queries;
    request_t **reqs = NULL;
    size_t      nreq = 0, capreq = 0;
    for (uint32_t id = 0; id < 65536; ++id) {
      query_t *q = id_del(m, (uint16_t)id);
      if (!q) continue;
      request_t *r = q->req;
      query_destroy(q);
      int seen = 0;
      for (size_t i = 0; i < nreq; ++i)
        if (reqs[i] == r) {
          seen = 1;
          break;
        }
      if (!seen) {
        if (nreq == capreq) {
          capreq = capreq ? capreq * 2 : 8;
          reqs   = (request_t **)realloc(reqs, capreq * sizeof(*reqs));
        }
        reqs[nreq++] = r;
      }
    }
    for (size_t i = 0; i < nreq; ++i) {
      request_t *r = reqs[i];
      dns_records_free(r->results);
      free(r);
    }
    free(reqs);
    xMapDestroy(m);
  }

  if (c->cache) dns_cache_destroy(c->cache);
  if (c->hosts) hosts_destroy(c->hosts);
  conns_cleanup(c);
  free(c);
}

void xDnsClientReloadHosts(xDnsClient client) {
  if (!client) return;
  struct xDnsClient_ *c = (struct xDnsClient_ *)client;
  if (!c->enable_hosts) return;
  if (c->hosts) hosts_destroy(c->hosts);
  c->hosts = hosts_load();
}

/* ───────────────────── Query lifecycle ───────────────────── */

static void query_destroy(query_t *q) {
  if (!q) return;
  if (q->timer) {
    xTimerStop(q->timer);
    q->timer = NULL;
  }
  free(q);
}

static xErrno send_query(struct xDnsClient_ *c, query_t *q) {
  conn_t *conn = conn_ready(c, q->ns_index);
  if (!conn) return xErrno_SysError;

  uint8_t buf[512];
  int n = dns_build_query(buf, sizeof(buf), q->id, q->name, q->qtype);
  if (n < 0) return xErrno_DnsError;

  struct sockaddr_in6     mapped;
  socklen_t               addrlen;
  const struct sockaddr  *addr = resolve_ns(c, q->ns_index, &mapped, &addrlen);
  ssize_t s = xSocketSendTo(conn->sock, buf, (size_t)n, addr, addrlen);
  if (s < 0) return xErrno_SysError;

  conn->query_count++;
  return xErrno_Ok;
}

static void on_query_timeout(void *arg) {
  query_t *q = (query_t *)arg;
  q->timer = NULL;

  struct xDnsClient_ *c = (struct xDnsClient_ *)q->req->client;

  if (q->retries_left > 0) {
    --q->retries_left;
    /* If multiple nameservers, rotate to next. Single nameserver: retry same. */
    if (c->ns_count > 1) q->ns_index = (q->ns_index + 1) % c->ns_count;
    if (send_query(c, q) == xErrno_Ok) {
      /* Exponential backoff: 2x timeout for subsequent retries */
      int to = c->retries - q->retries_left > 1 ? c->timeout_ms * 2 : c->timeout_ms;
      q->timer = xTimerStart(on_query_timeout, q, (uint64_t)to, 0);
      return;
    }
    /* fall through to failure */
  }

  /* Final failure for this query. */
  id_del(c->queries, q->id);
  request_t *req = q->req;
  req->last_err = xErrno_Timeout;
  /* detach q from req->queries */
  if (req->queries == q) req->queries = q->next;
  else {
    for (query_t *p = req->queries; p; p = p->next) {
      if (p->next == q) {
        p->next = q->next;
        break;
      }
    }
  }
  query_destroy(q);
  --req->pending;
  request_maybe_complete(req);
}

static void request_maybe_complete(request_t *req) {
  if (req->pending > 0) return;

  xErrno           err = req->had_success ? xErrno_Ok : req->last_err;
  const xDnsRecord *recs = req->had_success ? req->results : NULL;
  xDnsCallback     cb   = req->cb;
  void            *arg  = req->arg;

  /* Cache successful results (per-qtype). */
  if (req->had_success && recs) {
    struct xDnsClient_ *c = (struct xDnsClient_ *)req->client;
    if (c->cache) {
      /* Insert per-name+qtype entries. Each record's name may differ
       * (CNAME chains); group by qtype using the request's query types. */
      /* Simple grouping: iterate records, insert each unique (name,qtype). */
      for (const xDnsRecord *r = recs; r; r = r->next) {
        dns_cache_insert(c->cache, r->name, r->qtype, r, r->ttl ? r->ttl : 60);
      }
    }
  }

  cb(err, recs, arg);

  /* Free everything. */
  dns_records_free(req->results);
  free(req);
}

/* ───────────────────── Resolve ───────────────────── */

static void lowercase(char *s) {
  for (; *s; ++s) {
    if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
  }
}

xErrno xDnsClientDo(xDnsClient client, const char *name, xDnsType type,
                    xDnsCallback cb, void *arg) {
  if (!client || !name || !cb || type == 0) return xErrno_InvalidArg;
  struct xDnsClient_ *c = (struct xDnsClient_ *)client;

  char lname[256];
  strncpy(lname, name, sizeof(lname) - 1);
  lname[sizeof(lname) - 1] = '\0';
  lowercase(lname);

  /* Hosts file lookup: check before cache and DNS. */
  if (c->hosts) {
    xDnsType t = type;
    uint16_t qt = dns_qtype_take_next(&t);
    xDnsRecord *rec = hosts_lookup(c->hosts, lname, qt);
    if (rec) {
      cb(xErrno_Ok, rec, arg);
      return xErrno_Ok;
    }
  }

  /* Cache check: if every requested type is cached, merge and return. */
  if (c->cache) {
    xDnsType    remaining = type;
    xDnsRecord *merged = NULL, *merged_tail = NULL;
    int         all_hit = 1;
    for (;;) {
      uint16_t qt = dns_qtype_take_next(&remaining);
      if (qt == 0) break;
      xDnsRecord *hit = dns_cache_lookup(c->cache, lname, qt);
      if (!hit) {
        all_hit = 0;
        /* continue checking others? We need to send queries for misses.
         * Simpler: if any miss, fall back to sending all. Free what we
         * collected and proceed to network. */
        dns_records_free(merged);
        merged = NULL;
        break;
      }
      if (!merged) merged = hit;
      else         merged_tail->next = hit;
      while (hit->next) hit = hit->next;
      merged_tail = hit;
    }
    if (all_hit && merged) {
      cb(xErrno_Ok, merged, arg);
      dns_records_free(merged);
      return xErrno_Ok;
    }
    if (merged) dns_records_free(merged);
  }

  request_t *req = (request_t *)calloc(1, sizeof(request_t));
  if (!req) return xErrno_NoMemory;
  req->client   = client;
  req->cb       = cb;
  req->arg      = arg;
  req->last_err = xErrno_Timeout;

  xDnsType remaining = type;
  query_t **qtail = &req->queries;
  for (;;) {
    uint16_t qt = dns_qtype_take_next(&remaining);
    if (qt == 0) break;

    query_t *q = (query_t *)calloc(1, sizeof(query_t));
    if (!q) {
      req->last_err = xErrno_NoMemory;
      break;
    }
    q->qtype        = qt;
    q->ns_index     = 0;
    q->retries_left = c->retries;
    q->req          = req;
    strncpy(q->name, lname, sizeof(q->name) - 1);
    q->name[sizeof(q->name) - 1] = '\0';

    uint16_t id = alloc_id(c);
    if (id == 0) {
      free(q);
      req->last_err = xErrno_Unknown;
      break;
    }
    q->id = id;
    id_set(c->queries, id, q);

    if (send_query(c, q) != xErrno_Ok) {
      id_del(c->queries, id);
      free(q);
      req->last_err = xErrno_SysError;
      continue;
    }

    q->timer = xTimerStart(on_query_timeout, q, (uint64_t)c->timeout_ms, 0);
    *qtail = q;
    qtail  = &q->next;
    ++req->pending;
  }

  /* If nothing was submitted, complete the request inline. */
  if (req->pending == 0) {
    cb(req->last_err, NULL, arg);
    free(req);
    return xErrno_Ok;
  }

  return xErrno_Ok;
}

/* ───────────────────── Readable callback ───────────────────── */

static void on_readable(xSocket sock, xEventMask mask, void *arg) {
  if (!(mask & xEvent_Read)) return;
  struct xDnsClient_ *c = (struct xDnsClient_ *)arg;
  conn_t *conn = conn_find(c, sock);
  if (!conn) return;

  uint8_t buf[DNS_EDNS0_SIZE + 1];
  struct sockaddr_storage src;
  socklen_t               srclen;

  for (;;) {
    srclen = sizeof(src);
    ssize_t n = xSocketRecvFrom(conn->sock, buf, sizeof(buf),
                                (struct sockaddr *)&src, &srclen);
    if (n < 0) break; /* EAGAIN / error — stop draining */

    dns_header_t   hdr;
    dns_question_t q;
    xDnsRecord    *answers = NULL;
    if (dns_parse((const uint8_t *)buf, (size_t)n, &hdr, &q, &answers) != xErrno_Ok)
      continue;

    query_t *qx = id_del(c->queries, hdr.id);
    if (!qx) {
      dns_records_free(answers);
      continue;
    }

    request_t *req = qx->req;

    /* Stop this query's timer. */
    if (qx->timer) {
      xTimerStop(qx->timer);
      qx->timer = NULL;
    }

    /* detach qx from req->queries */
    if (req->queries == qx) req->queries = qx->next;
    else {
      for (query_t *p = req->queries; p; p = p->next) {
        if (p->next == qx) {
          p->next = qx->next;
          break;
        }
      }
    }

    int rcode = hdr.flags & DNS_RCODE_MASK;
    xErrno qerr = xErrno_Ok;
    if (rcode == 3)      qerr = xErrno_DnsNotFound;
    else if (rcode != 0) qerr = xErrno_DnsError;

    if (qerr == xErrno_Ok) {
      /* success — append answers (if any) to request results. */
      req->had_success = 1;
      if (answers) {
        if (!req->results)      req->results      = answers;
        else                    req->results_tail->next = answers;
        xDnsRecord *t = answers;
        while (t->next) t = t->next;
        req->results_tail = t;
      }
    } else {
      req->last_err = qerr;
      dns_records_free(answers);
    }

    query_destroy(qx);
    --req->pending;
    request_maybe_complete(req);
  }
}
