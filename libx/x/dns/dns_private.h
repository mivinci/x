/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns_private.h - Internal helpers shared by dns_client.c and dns_server.c
 */

#ifndef XDNS_DNS_PRIVATE_H
#define XDNS_DNS_PRIVATE_H

#include <stddef.h>
#include <stdint.h>

#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>
#include <x/base/map.h>
#include <x/dns/dns.h>

/* ───────────────────── Wire constants ───────────────────── */

#define DNS_PORT       53
#define DNS_EDNS0_SIZE 4096u /**< UDP payload size advertised via OPT */

/* DNS QTYPE values (on-the-wire) */
#define DNS_QTYPE_A     1u
#define DNS_QTYPE_CNAME 5u
#define DNS_QTYPE_AAAA  28u
#define DNS_QTYPE_OPT   41u /**< EDNS0 OPT pseudo-record */

#define DNS_QCLASS_IN 1u

/* Header flags */
#define DNS_FLAG_QR        0x8000u
#define DNS_FLAG_RD        0x0100u
#define DNS_FLAG_RA        0x0080u
#define DNS_FLAG_AA        0x0400u
#define DNS_RCODE_MASK     0x000Fu
#define DNS_RCODE_NXDOMAIN 3u

/* ───────────────────── Types ───────────────────── */

/** Decoded DNS header (host byte order). */
typedef struct {
  uint16_t id;
  uint16_t flags;
  uint16_t qdcount;
  uint16_t ancount;
  uint16_t nscount;
  uint16_t arcount;
} dns_header_t;

/** Decoded DNS question. */
typedef struct {
  char     name[256]; /**< Decoded QNAME, NUL-terminated                  */
  uint16_t qtype;
  uint16_t qclass;
  size_t   wire_off; /**< Offset in the packet just past this question   */
} dns_question_t;

/* ───────────────────── Packet builder ───────────────────── */

/**
 * Build a DNS query packet (header + question + EDNS0 OPT record).
 *
 * @param buf     Destination buffer.
 * @param buflen  Capacity of @p buf.
 * @param id      Transaction ID.
 * @param name    QNAME (e.g. "example.com").
 * @param qtype   Wire QTYPE (DNS_QTYPE_A / DNS_QTYPE_AAAA / DNS_QTYPE_CNAME).
 * @return        Packet length on success, or -1 on error (buffer too small
 *                or malformed name).
 */
XCAPI(int) dns_build_query(uint8_t *buf, size_t buflen, uint16_t id, const char *name,
                           uint16_t qtype);

/**
 * Build a DNS response packet.
 *
 * @param buf           Destination buffer.
 * @param buflen        Capacity of @p buf.
 * @param id            Transaction ID (copied from query).
 * @param rcode         Response code (0=NOERROR, 3=NXDOMAIN, ...).
 * @param qname         QNAME to echo back in the question section.
 * @param qtype         QTYPE to echo back.
 * @param answers       Linked list of answer records, or NULL.
 * @return              Packet length on success, or -1 on error.
 */
XCAPI(int) dns_build_response(uint8_t *buf, size_t buflen, uint16_t id, int rcode,
                              const char *qname, uint16_t qtype, const xDnsRecord *answers);

/* ───────────────────── Packet parser ───────────────────── */

/**
 * Parse a DNS packet.
 *
 * @param buf       Packet bytes.
 * @param len       Packet length.
 * @param hdr       Out: decoded header (must not be NULL).
 * @param q         Out: first question (must not be NULL). Skipped if
 *                  qdcount == 0.
 * @param answers   Out: linked list of answer records (must not be NULL).
 *                  Set to NULL on error or no answers. Caller frees with
 *                  dns_records_free().
 * @return          xErrno_Ok on success, or an error code.
 */
XCAPI(xErrno) dns_parse(const uint8_t *buf, size_t len, dns_header_t *hdr, dns_question_t *q,
                        xDnsRecord **answers);

/* ───────────────────── Record helpers ───────────────────── */

/** Free a linked list of xDnsRecord (as produced by dns_parse). */
XCAPI(void) dns_records_free(xDnsRecord *rec);

/** Deep-clone a linked list of xDnsRecord. Returns NULL on OOM. */
XCAPI(xDnsRecord *) dns_records_clone(const xDnsRecord *rec);

/** Map an xDnsType bitmask bit to a wire QTYPE. Returns 0 if unknown. */
XCAPI(uint16_t) dns_qtype_from_bit(xDnsType type);

/**
 * Pick the next single-bit QTYPE from a (possibly multi-bit) xDnsType mask.
 * Clears the bit in @p *type and returns the corresponding wire QTYPE, or 0
 * when the mask is empty.
 */
XCAPI(uint16_t) dns_qtype_take_next(xDnsType *type);

/* ───────────────────── Cache ───────────────────── */

/**
 * Create a TTL cache. Returns an xMap handle.
 */
XCAPI(xMap) dns_cache_create(void);

/**
 * Destroy a cache, freeing all keys, values, and the map itself.
 * Safe to call with NULL.
 */
XCAPI(void) dns_cache_destroy(xMap cache);

/**
 * Look up a cached entry.
 *
 * @param cache  The cache map.
 * @param name   Hostname.
 * @param qtype  Wire QTYPE.
 * @return       A freshly cloned xDnsRecord list (caller frees with
 *               dns_records_free) if a fresh entry exists, or NULL on
 *               miss / expiry. Expired entries are evicted.
 */
XCAPI(xDnsRecord *) dns_cache_lookup(xMap cache, const char *name, uint16_t qtype);

/**
 * Insert (or replace) a cache entry. @p records is deep-cloned.
 *
 * @param cache   The cache map.
 * @param name    Hostname.
 * @param qtype   Wire QTYPE.
 * @param records Records to cache (may be NULL — inserts an empty entry).
 * @param ttl     TTL in seconds.
 */
XCAPI(void) dns_cache_insert(xMap cache, const char *name, uint16_t qtype,
                             const xDnsRecord *records, uint32_t ttl);

/* ───────────────────── Config ───────────────────── */

/**
 * Discover system nameservers.
 *
 * On POSIX: parses /etc/resolv.conf. On Windows: uses GetNetworkParams().
 * Falls back to "8.8.8.8" on any failure. Writes up to @p max entries to
 * @p out (each buffer is at least 46 bytes). Returns the count written.
 *
 * The strings written to @p out are owned by the caller's buffer.
 */
XCAPI(int) dns_config_load_nameservers(char out[][46], int max);

#endif /* XDNS_DNS_PRIVATE_H */
