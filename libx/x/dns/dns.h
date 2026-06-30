/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns.h - Truly async DNS client + server over UDP
 *
 * Implements the DNS protocol (RFC 1035) directly over UDP with no
 * getaddrinfo() and no thread pool. The client uses a single non-blocking
 * UDP socket registered with the event loop and multiplexes concurrent
 * queries by 16-bit transaction ID. The server supports authoritative
 * zones, forwarding, and a query filter callback.
 *
 * Depends only on xbase (xSocket, xTimer, xMap, xEventLoop).
 *
 * V1 supports A, AAAA, and CNAME record types, EDNS(0) OPT records
 * (RFC 6891) advertising a 4096-byte UDP payload size, and DNS name
 * compression (RFC 1035 §4.1.4) in the parser.
 */

#ifndef XDNS_DNS_H
#define XDNS_DNS_H

#include <stddef.h>
#include <stdint.h>

#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>

/* ───────────────────── Types ───────────────────── */

/**
 * @brief Bitmask of DNS record types to query for.
 *
 * Callers can OR multiple values together in a single xDnsClientDo()
 * request; the client sends one UDP query per bit and merges the results
 * into a single xDnsRecord list before invoking the callback once.
 *
 * These bitmask values are distinct from the on-the-wire DNS QTYPE values
 * (1, 28, 5) stored in xDnsRecord.qtype.
 */
XDEF_ENUM(xDnsType){
  xDnsType_A     = 1 << 0, /**< IPv4 address          → QTYPE=1  */
  xDnsType_AAAA  = 1 << 1, /**< IPv6 address          → QTYPE=28 */
  xDnsType_CNAME = 1 << 2, /**< Canonical alias       → QTYPE=5  */
};

/**
 * @brief A single DNS resource record.
 *
 * Returned as a singly-linked list by xDnsClientDo(). All memory is owned
 * by the library and is valid only for the duration of the callback; copy
 * if needed beyond that.
 *
 * @p qtype holds the on-the-wire DNS QTYPE (1=A, 28=AAAA, 5=CNAME), not
 * the xDnsType bitmask.
 */
XDEF_STRUCT(xDnsRecord) {
  uint16_t    qtype;    /**< DNS QTYPE (1=A, 28=AAAA, 5=CNAME)         */
  uint32_t    ttl;      /**< TTL in seconds (from the response)         */
  const char *name;     /**< Owner name (NUL-terminated, lowercase)     */
  const void *rdata;    /**< Raw RDATA                                  */
  size_t      rdlength; /**< Length of @p rdata in bytes                */
  xDnsRecord *next;     /**< Next record in the list, or NULL           */
};

/* ───────────────────── Client ───────────────────── */

/**
 * @brief Opaque handle to an async DNS client.
 */
XDEF_HANDLE(xDnsClient);

/**
 * @brief Configuration for creating a DNS client.
 *
 * Zero-initialize for defaults: nameservers discovered from the system
 * (or "8.8.8.8" as fallback), 5000ms timeout, 2 retries, cache enabled.
 */
XDEF_STRUCT(xDnsClientConf) {
  const char *nameservers[8]; /**< Up to 8 nameservers ("8.8.8.8", ...).
                                   NULL-terminated. If nameservers[0]
                                   is NULL, system config is used.        */
  int timeout_ms;             /**< Per-query timeout (default 5000).       */
  int retries;                /**< Retries with next nameserver (default 2).*/
  int enable_cache;           /**< 1=enable TTL cache (default), 0=disable.*/
  int udp_max_queries;        /**< Max queries per UDP connection before
                                   rotating to a new source port.
                                   0 = unlimited (default).             */
  int enable_hosts;           /**< 1=load and use /etc/hosts (default),
                                     0=disable.                           */
};

/**
 * @brief Completion callback for xDnsClientDo().
 *
 * Invoked exactly once per request, on the event loop thread, after all
 * constituent queries complete (or time out). On success, @p records is
 * a (possibly empty) linked list of results. On error, @p records is NULL.
 *
 * @param err      xErrno_Ok on success (even if no records), or an error
 *                 code (xErrno_Timeout, xErrno_DnsNotFound, ...).
 * @param records  Linked list of records, or NULL on error. Valid only
 *                 during the callback.
 * @param arg      User-provided argument.
 */
typedef void (*xDnsCallback)(xErrno err, const xDnsRecord *records, void *arg);

/**
 * @brief Create a DNS client bound to the current event loop.
 *
 * @param conf  Configuration, or NULL for defaults.
 * @return      A new client handle, or NULL on failure.
 */
XCAPI(xDnsClient) xDnsClientCreate(const xDnsClientConf *conf);

/**
 * @brief Destroy a DNS client and release all resources.
 *
 * In-flight queries are cancelled; their callbacks are NOT invoked.
 * Safe to call with NULL.
 */
XCAPI(void) xDnsClientDestroy(xDnsClient client);

/**
 * @brief Reload /etc/hosts into the client's hosts table.
 *
 * Frees the old table and re-parses the hosts file. Does nothing if
 * hosts support was disabled at creation time (enable_hosts = 0).
 * Safe to call with NULL.
 *
 * @param client  The DNS client.
 */
XCAPI(void) xDnsClientReloadHosts(xDnsClient client);

/**
 * @brief Resolve a hostname asynchronously.
 *
 * For each bit set in @p type, a separate UDP query is sent. When all
 * queries for this request complete (or time out), @p cb is invoked once
 * with the merged results.
 *
 * If the cache is enabled and a fresh entry exists, @p cb is invoked
 * immediately (still on the event loop thread, via a zero-timer) with the
 * cached records.
 *
 * @param client  The DNS client.
 * @param name    Hostname to resolve (must not be NULL).
 * @param type    Bitmask of xDnsType_A / xDnsType_AAAA / xDnsType_CNAME.
 * @param cb      Completion callback (must not be NULL).
 * @param arg     User argument forwarded to @p cb.
 * @return        xErrno_Ok on success, xErrno_InvalidArg on bad args.
 */
XCAPI(xErrno) xDnsClientDo(xDnsClient client, const char *name, xDnsType type, xDnsCallback cb,
                           void *arg);

/* ───────────────────── Server ───────────────────── */

/**
 * @brief Opaque handle to a DNS zone (a collection of records).
 *
 * Declared early so xDnsServerConf / xDnsServerAddZone can refer to it.
 */
XDEF_HANDLE(xDnsZone);

/**
 * @brief Opaque handle to a DNS server.
 */
XDEF_HANDLE(xDnsServer);

/**
 * @brief Filter callback: inspect and possibly block an incoming query.
 *
 * Invoked on the event loop thread before zone lookup or forwarding.
 * Return 0 to allow normal processing; return non-zero to drop the query
 * (the server responds with NXDOMAIN).
 *
 * @param name  Query name (NUL-terminated, lowercase).
 * @param type  DNS QTYPE of the query (1, 28, 5, ...).
 * @param arg   User-provided argument.
 * @return      0 = allow, non-zero = block.
 */
typedef int (*xDnsFilterFunc)(const char *name, uint16_t type, void *arg);

/**
 * @brief Configuration for creating a DNS server.
 *
 * Zero-initialize for defaults: no forwarder, no filter, cache disabled.
 */
XDEF_STRUCT(xDnsServerConf) {
  xDnsClient forwarder;         /**< Upstream resolver, or NULL
                                     (authoritative-only).                */
  xDnsFilterFunc filter;        /**< Query filter, or NULL.               */
  void          *filter_arg;    /**< Argument forwarded to @p filter.     */
  int            cache_enabled; /**< 1=share cache with forwarder.        */
};

/**
 * @brief Create a DNS server bound to the current event loop.
 * @param conf  Configuration, or NULL for defaults.
 * @return      A new server handle, or NULL on failure.
 */
XCAPI(xDnsServer) xDnsServerCreate(const xDnsServerConf *conf);

/**
 * @brief Destroy a DNS server and release all resources. Safe to call NULL.
 */
XCAPI(void) xDnsServerDestroy(xDnsServer server);

/**
 * @brief Start listening for DNS queries on the given UDP port.
 *
 * @param server  The DNS server.
 * @param host    Bind address (e.g. "127.0.0.1"), or NULL for "0.0.0.0".
 * @param port    UDP port (e.g. 53, or 0 for an ephemeral port).
 * @return        xErrno_Ok on success.
 */
XCAPI(xErrno) xDnsServerListen(xDnsServer server, const char *host, uint16_t port);

/**
 * @brief Return the actual bound UDP port (useful when @p port was 0).
 * @return Port, or 0 if not listening / on error.
 */
XCAPI(uint16_t) xDnsServerPort(xDnsServer server);

/**
 * @brief Attach a zone to the server. The server checks all zones in
 *        registration order; first match wins.
 */
XCAPI(xErrno) xDnsServerAddZone(xDnsServer server, xDnsZone zone);

/* ───────────────────── Zone API ───────────────────── */

/**
 * @brief Create an empty zone.
 */
XCAPI(xDnsZone) xDnsZoneCreate(void);

/**
 * @brief Destroy a zone and free all records. Safe to call NULL.
 *
 * Removing a zone from a server before destroying it is the caller's
 * responsibility; xDnsServerDestroy does not free zones.
 */
XCAPI(void) xDnsZoneDestroy(xDnsZone zone);

/**
 * @brief Add a record to the zone.
 *
 * @param zone   The zone.
 * @param name   Owner name (e.g. "myapp.local"). Copied; caller retains
 *               ownership of the input.
 * @param type   Record type as xDnsType bitmask. Exactly one bit must be
 *               set (xDnsType_A, xDnsType_AAAA, or xDnsType_CNAME).
 * @param rdata  Raw RDATA (e.g. 4 bytes for A, 16 bytes for AAAA, or a
 *               NUL-terminated domain name for CNAME). Copied.
 * @param rdlen  Length of @p rdata in bytes.
 * @param ttl    TTL in seconds.
 * @return       xErrno_Ok on success.
 */
XCAPI(xErrno) xDnsZoneAdd(xDnsZone zone, const char *name, xDnsType type, const void *rdata,
                          size_t rdlen, uint32_t ttl);

#endif /* XDNS_DNS_H */
