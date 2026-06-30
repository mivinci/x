/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns_packet.c - DNS packet build / parse (RFC 1035 + RFC 6891 EDNS0)
 */

#include "dns_private.h"

#include <stdlib.h>
#include <string.h>

/* ───────────────────── Byte helpers ───────────────────── */

static void put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xFF);
}

static uint16_t get_u16(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ───────────────────── Name encoding ───────────────────── */

/*
 * Encode "example.com" as a DNS QNAME: \x07example\x03com\x00.
 * Returns the encoded length (excluding terminating 0x00 if you count
 * labels), or -1 on error. Writes into @p out (capacity @p outcap).
 */
static int encode_name(uint8_t *out, size_t outcap, const char *name) {
  size_t i           = 0; /* write position in @p out */
  size_t label_start = 0;
  size_t j;

  for (j = 0;; ++j) {
    char c = name[j];
    if (c == '.' || c == '\0') {
      size_t lablen = j - label_start;
      if (lablen == 0) {
        /* empty label (leading dot, trailing dot, or double dot) */
        if (c == '\0' && label_start == 0 && j == 0) {
          /* empty name → root */
          break;
        }
        /* reject embedded empty labels */
        return -1;
      }
      if (lablen > 63) return -1;
      if (i + 1 + lablen > outcap) return -1;
      out[i++] = (uint8_t)lablen;
      memcpy(out + i, name + label_start, lablen);
      i += lablen;
      label_start = j + 1;
      if (c == '\0') break;
    }
  }
  if (i + 1 > outcap) return -1;
  out[i++] = 0; /* root terminator */
  return (int)i;
}

/*
 * Parse a domain name starting at offset @p pos in @p buf (length @p len).
 * Writes the decoded name (NUL-terminated) to @p out (capacity @p out_cap).
 * Follows compression pointers (RFC 1035 §4.1.4).
 *
 * Returns the offset in @p buf just past the name (i.e. past the terminating
 * 0x00 or past the 2-byte pointer), or -1 on error (truncated, bad label
 * length, loop).
 */
static int parse_name(const uint8_t *buf, size_t len, size_t pos, char *out, size_t out_cap) {
  size_t out_i    = 0;
  size_t cur      = pos;
  int    followed = 0; /* did we follow a pointer? */
  size_t end_off  = 0; /* offset past the name (set on first pointer) */
  size_t seen[64];     /* visited offsets for loop detection */
  size_t seen_n      = 0;
  int    wrote_label = 0;

  for (;;) {
    if (cur >= len) return -1;
    uint8_t c = buf[cur];

    if ((c & 0xC0) == 0xC0) {
      /* compression pointer */
      if (cur + 1 >= len) return -1;
      size_t target = (size_t)((c & 0x3F) << 8) | buf[cur + 1];
      if (!followed) {
        end_off  = cur + 2;
        followed = 1;
      }
      /* loop detection */
      for (size_t i = 0; i < seen_n; ++i)
        if (seen[i] == target) return -1;
      if (seen_n >= sizeof(seen) / sizeof(seen[0])) return -1;
      seen[seen_n++] = target;
      cur            = target;
      continue;
    }

    if ((c & 0xC0) == 0x80 || (c & 0xC0) == 0x40) {
      /* reserved label types (RFC 1035 §4.1.4) — not supported in V1 */
      return -1;
    }

    /* normal label: c is the length */
    if (c == 0) {
      /* root terminator */
      if (!followed) end_off = cur + 1;
      if (out_i == 0) {
        /* root domain — empty string is fine */
      } else {
        /* strip trailing dot already written */
        if (out_i > 0 && out[out_i - 1] == '.') out_i--;
      }
      if (out_i >= out_cap) return -1;
      out[out_i] = '\0';
      (void)wrote_label;
      return (int)end_off;
    }

    /* normal label of length c */
    size_t lablen = c;
    if (cur + 1 + lablen > len) return -1;
    if (out_i + lablen + 1 >= out_cap) return -1;
    memcpy(out + out_i, buf + cur + 1, lablen);
    out_i += lablen;
    out[out_i++] = '.';
    wrote_label  = 1;
    cur += 1 + lablen;
  }
}

/* ───────────────────── Query builder ───────────────────── */

int dns_build_query(uint8_t *buf, size_t buflen, uint16_t id, const char *name, uint16_t qtype) {
  if (!buf || !name) return -1;

  /* 12 (header) + name + 4 (qtype+qclass) + 11 (OPT record) */
  size_t need = 12 + 256 + 4 + 11;
  if (buflen < need) return -1;

  size_t off = 0;

  /* Header */
  put_u16(buf + off, id);
  off += 2;
  put_u16(buf + off, DNS_FLAG_RD);
  off += 2; /* RD=1, standard query */
  put_u16(buf + off, 1);
  off += 2; /* QDCOUNT=1 */
  put_u16(buf + off, 0);
  off += 2; /* ANCOUNT=0 */
  put_u16(buf + off, 0);
  off += 2; /* NSCOUNT=0 */
  put_u16(buf + off, 1);
  off += 2; /* ARCOUNT=1 (OPT) */

  /* Question: QNAME */
  int nlen = encode_name(buf + off, buflen - off, name);
  if (nlen < 0) return -1;
  off += (size_t)nlen;

  /* Question: QTYPE + QCLASS */
  put_u16(buf + off, qtype);
  off += 2;
  put_u16(buf + off, DNS_QCLASS_IN);
  off += 2;

  /* Additional: EDNS0 OPT record (RFC 6891), 11 bytes total */
  buf[off++] = 0; /* NAME: root */
  put_u16(buf + off, DNS_QTYPE_OPT);
  off += 2; /* TYPE=OPT */
  put_u16(buf + off, DNS_EDNS0_SIZE);
  off += 2; /* CLASS=UDP payload size */
  put_u16(buf + off, 0);
  off += 2; /* TTL bytes 0-1: ext-rcode=0, version=0 */
  put_u16(buf + off, 0);
  off += 2; /* TTL bytes 2-3: flags (DO=0) */
  put_u16(buf + off, 0);
  off += 2; /* RDLENGTH=0 */

  return (int)off;
}

/* ───────────────────── Response builder ───────────────────── */

int dns_build_response(uint8_t *buf, size_t buflen, uint16_t id, int rcode, const char *qname,
                       uint16_t qtype, const xDnsRecord *answers) {
  if (!buf || !qname) return -1;

  /* Count answers and compute exact required size. */
  uint16_t ancount = 0;
  size_t   need    = 12 + (strlen(qname) + 2) + 4; /* header + question */
  for (const xDnsRecord *r = answers; r; r = r->next) {
    ++ancount;
    need += (r->name ? strlen(r->name) : 0) + 2 + 10 + r->rdlength;
  }
  if (buflen < need) return -1;

  size_t off = 0;

  /* Header */
  uint16_t flags = DNS_FLAG_QR | DNS_FLAG_RA | (uint16_t)(rcode & DNS_RCODE_MASK);
  put_u16(buf + off, id);
  off += 2;
  put_u16(buf + off, flags);
  off += 2;
  put_u16(buf + off, 1);
  off += 2; /* QDCOUNT=1 */
  put_u16(buf + off, ancount);
  off += 2;
  put_u16(buf + off, 0);
  off += 2; /* NSCOUNT=0 */
  put_u16(buf + off, 0);
  off += 2; /* ARCOUNT=0 */

  /* Question (echo) */
  int nlen = encode_name(buf + off, buflen - off, qname);
  if (nlen < 0) return -1;
  off += (size_t)nlen;
  put_u16(buf + off, qtype);
  off += 2;
  put_u16(buf + off, DNS_QCLASS_IN);
  off += 2;

  /* Answers */
  for (const xDnsRecord *r = answers; r; r = r->next) {
    /* NAME: echo r->name (no compression for simplicity) */
    int rlen = encode_name(buf + off, buflen - off, r->name ? r->name : qname);
    if (rlen < 0) return -1;
    off += (size_t)rlen;
    put_u16(buf + off, r->qtype);
    off += 2;
    put_u16(buf + off, DNS_QCLASS_IN);
    off += 2;
    put_u16(buf + off, (uint16_t)(r->ttl >> 16));
    off += 2;
    put_u16(buf + off, (uint16_t)(r->ttl & 0xFFFF));
    off += 2;

    /* For CNAME, RDATA is a domain name that must be DNS-encoded. The
     * caller passes it as a NUL-terminated string in r->rdata. */
    if (r->qtype == DNS_QTYPE_CNAME) {
      const char *cname = r->rdata ? (const char *)r->rdata : "";
      /* Reserve 2 bytes for RDLENGTH, encode name, then backfill. */
      size_t rdlen_pos = off;
      off += 2;
      int clen = encode_name(buf + off, buflen - off, cname);
      if (clen < 0) return -1;
      put_u16(buf + rdlen_pos, (uint16_t)clen);
      off += (size_t)clen;
    } else {
      put_u16(buf + off, (uint16_t)r->rdlength);
      off += 2;
      if (r->rdlength > 0 && r->rdata) {
        if (off + r->rdlength > buflen) return -1;
        memcpy(buf + off, r->rdata, r->rdlength);
        off += r->rdlength;
      }
    }
  }

  return (int)off;
}

/* ───────────────────── Parser ───────────────────── */

xErrno dns_parse(const uint8_t *buf, size_t len, dns_header_t *hdr, dns_question_t *q,
                 xDnsRecord **answers) {
  if (!buf || !hdr || !q || !answers) return xErrno_InvalidArg;
  *answers = NULL;

  if (len < 12) return xErrno_InvalidArg;

  hdr->id      = get_u16(buf + 0);
  hdr->flags   = get_u16(buf + 2);
  hdr->qdcount = get_u16(buf + 4);
  hdr->ancount = get_u16(buf + 6);
  hdr->nscount = get_u16(buf + 8);
  hdr->arcount = get_u16(buf + 10);

  size_t off = 12;

  /* Question section */
  q->name[0]  = '\0';
  q->qtype    = 0;
  q->qclass   = 0;
  q->wire_off = off;
  if (hdr->qdcount > 0) {
    int nend = parse_name(buf, len, off, q->name, sizeof(q->name));
    if (nend < 0) return xErrno_DnsError;
    off = (size_t)nend;
    if (off + 4 > len) return xErrno_DnsError;
    q->qtype = get_u16(buf + off);
    off += 2;
    q->qclass = get_u16(buf + off);
    off += 2;
    q->wire_off = off;
  }

  /* Answer section */
  xDnsRecord *tail = NULL;
  xDnsRecord *head = NULL;
  for (uint16_t i = 0; i < hdr->ancount; ++i) {
    char owner[256];
    int  nend = parse_name(buf, len, off, owner, sizeof(owner));
    if (nend < 0) goto fail;
    off = (size_t)nend;
    if (off + 10 > len) goto fail;
    uint16_t rtype = get_u16(buf + off);
    off += 2;
    uint16_t rclass = get_u16(buf + off);
    off += 2;
    uint32_t ttl = get_u32(buf + off);
    off += 4;
    uint16_t rdlen = get_u16(buf + off);
    off += 2;
    (void)rclass;
    if (off + rdlen > len) goto fail;

    /* Only decode record types we care about; skip OPT and others. */
    if (rtype == DNS_QTYPE_OPT) {
      off += rdlen;
      continue;
    }

    xDnsRecord *rec = (xDnsRecord *)calloc(1, sizeof(xDnsRecord));
    if (!rec) goto fail;
    rec->qtype    = rtype;
    rec->ttl      = ttl;
    rec->rdlength = rdlen;

    /* Copy owner name */
    {
      size_t nl = strlen(owner);
      char  *nm = (char *)malloc(nl + 1);
      if (!nm) {
        free(rec);
        goto fail;
      }
      memcpy(nm, owner, nl + 1);
      rec->name = nm;
    }

    /* For CNAME, decode the RDATA as a domain name (may use compression). */
    if (rtype == DNS_QTYPE_CNAME) {
      char cname[256];
      int  cend = parse_name(buf, len, off, cname, sizeof(cname));
      if (cend < 0) {
        free((void *)rec->name);
        free(rec);
        goto fail;
      }
      size_t cl = strlen(cname);
      char  *cd = (char *)malloc(cl + 1);
      if (!cd) {
        free((void *)rec->name);
        free(rec);
        goto fail;
      }
      memcpy(cd, cname, cl + 1);
      rec->rdata    = cd;
      rec->rdlength = cl + 1; /* expose NUL-terminated string for convenience */
    } else {
      /* A / AAAA / unknown: copy raw RDATA */
      if (rdlen > 0) {
        void *rd = malloc(rdlen);
        if (!rd) {
          free((void *)rec->name);
          free(rec);
          goto fail;
        }
        memcpy(rd, buf + off, rdlen);
        rec->rdata = rd;
      }
    }

    off += rdlen;

    if (!head)
      head = rec;
    else
      tail->next = rec;
    tail = rec;
  }

  *answers = head;
  return xErrno_Ok;

fail:
  dns_records_free(head);
  return xErrno_DnsError;
}

/* ───────────────────── Helpers ───────────────────── */

void dns_records_free(xDnsRecord *rec) {
  while (rec) {
    xDnsRecord *next = rec->next;
    free((void *)rec->name);
    free((void *)rec->rdata);
    free(rec);
    rec = next;
  }
}

xDnsRecord *dns_records_clone(const xDnsRecord *rec) {
  xDnsRecord *head = NULL, *tail = NULL;
  for (const xDnsRecord *r = rec; r; r = r->next) {
    xDnsRecord *c = (xDnsRecord *)calloc(1, sizeof(xDnsRecord));
    if (!c) goto fail;
    c->qtype    = r->qtype;
    c->ttl      = r->ttl;
    c->rdlength = r->rdlength;
    if (r->name) {
      size_t nl = strlen(r->name);
      char  *nm = (char *)malloc(nl + 1);
      if (!nm) {
        free(c);
        goto fail;
      }
      memcpy(nm, r->name, nl + 1);
      c->name = nm;
    }
    if (r->rdlength > 0 && r->rdata) {
      void *rd = malloc(r->rdlength);
      if (!rd) {
        free((void *)c->name);
        free(c);
        goto fail;
      }
      memcpy(rd, r->rdata, r->rdlength);
      c->rdata = rd;
    }
    if (!head)
      head = c;
    else
      tail->next = c;
    tail = c;
  }
  return head;
fail:
  dns_records_free(head);
  return NULL;
}

uint16_t dns_qtype_from_bit(xDnsType type) {
  switch (type) {
  case xDnsType_A:
    return DNS_QTYPE_A;
  case xDnsType_AAAA:
    return DNS_QTYPE_AAAA;
  case xDnsType_CNAME:
    return DNS_QTYPE_CNAME;
  default:
    return 0;
  }
}

uint16_t dns_qtype_take_next(xDnsType *type) {
  if (!type) return 0;
  xDnsType t = *type;
  if (t & xDnsType_A) {
    *type = (xDnsType)(t & ~xDnsType_A);
    return DNS_QTYPE_A;
  }
  if (t & xDnsType_AAAA) {
    *type = (xDnsType)(t & ~xDnsType_AAAA);
    return DNS_QTYPE_AAAA;
  }
  if (t & xDnsType_CNAME) {
    *type = (xDnsType)(t & ~xDnsType_CNAME);
    return DNS_QTYPE_CNAME;
  }
  return 0;
}
