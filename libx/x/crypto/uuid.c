/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * uuid.c - UUID generation (v4, v5, v7), formatting, and parsing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <x/base/random.h>
#include <x/base/time.h>
#include <x/crypto/sha1.h>
#include <x/crypto/uuid.h>

/* ── Helpers ────────────────────────────────────────────────────── */

static void set_version(xUuid *u, int version) {
  u->bytes[6] = (uint8_t)((u->bytes[6] & 0x0F) | (version << 4));
}

static void set_variant(xUuid *u) {
  u->bytes[8] = (uint8_t)((u->bytes[8] & 0x3F) | 0x80);
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* ── v4: Random ─────────────────────────────────────────────────── */

xUuid xUuidV4(void) {
  xUuid u;
  xRandomBytes(u.bytes, 16);
  set_version(&u, 4);
  set_variant(&u);
  return u;
}

/* ── v7: Time-ordered random (RFC 9562) ─────────────────────────── */

xUuid xUuidV7(void) {
  xUuid    u;
  uint64_t now_ms = xMonoMs();

  u.bytes[0] = (uint8_t)(now_ms >> 40);
  u.bytes[1] = (uint8_t)(now_ms >> 32);
  u.bytes[2] = (uint8_t)(now_ms >> 24);
  u.bytes[3] = (uint8_t)(now_ms >> 16);
  u.bytes[4] = (uint8_t)(now_ms >> 8);
  u.bytes[5] = (uint8_t)(now_ms);

  xRandomBytes(u.bytes + 6, 10);
  set_version(&u, 7);
  set_variant(&u);
  return u;
}

/* ── v5: Namespace + name (SHA-1) ──────────────────────────────── */

xUuid xUuidV5(xUuid ns, const char *name) {
  xUuid    result;
  uint8_t  hash[20];
  xSha1Ctx ctx;

  xSha1Init(&ctx);
  xSha1Update(&ctx, ns.bytes, 16);
  xSha1Update(&ctx, (const uint8_t *)name, strlen(name));
  xSha1Final(&ctx, hash);

  memcpy(result.bytes, hash, 16);
  set_version(&result, 5);
  set_variant(&result);
  return result;
}

/* ── Formatting ─────────────────────────────────────────────────── */

xErrno xUuidFromString(const char *str, xUuid *out) {
  if (!str || !out) return xErrno_InvalidArg;

  size_t len = strlen(str);
  if (len != 36 && len != 32) return xErrno_InvalidArg;

  int byte_idx = 0;
  for (size_t i = 0; i < len; i++) {
    if (len == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
      if (str[i] != '-') return xErrno_InvalidArg;
      continue;
    }
    int hi = hex_val(str[i]);
    if (hi < 0) return xErrno_InvalidArg;
    i++;
    if (i >= len) return xErrno_InvalidArg;
    int lo = hex_val(str[i]);
    if (lo < 0) return xErrno_InvalidArg;
    if (byte_idx >= 16) return xErrno_InvalidArg;
    out->bytes[byte_idx++] = (uint8_t)((hi << 4) | lo);
  }

  if (byte_idx != 16) return xErrno_InvalidArg;
  return xErrno_Ok;
}

void xUuidToString(xUuid uuid, char buf[37]) {
  static const char hex[] = "0123456789abcdef";
  int               bi = 0, si = 0;
  for (int group = 0; group < 5; group++) {
    int group_len = (group == 0) ? 4 : (group == 1) ? 2 : (group == 2) ? 2 : (group == 3) ? 2 : 6;
    for (int j = 0; j < group_len; j++) {
      uint8_t b = uuid.bytes[bi++];
      buf[si++] = hex[b >> 4];
      buf[si++] = hex[b & 0x0F];
    }
    if (group < 4) buf[si++] = '-';
  }
  buf[si] = '\0';
}

/* ── Comparison ─────────────────────────────────────────────────── */

int xUuidCompare(xUuid a, xUuid b) {
  return memcmp(a.bytes, b.bytes, 16);
}

bool xUuidIsNil(xUuid uuid) {
  for (int i = 0; i < 16; i++) {
    if (uuid.bytes[i] != 0) return false;
  }
  return true;
}

/* ── Predefined namespaces ──────────────────────────────────────── */

static const xUuid g_ns_dns = {{
  0x6b,
  0xa7,
  0xb8,
  0x10,
  0x9d,
  0xad,
  0x11,
  0xd1,
  0x80,
  0xb4,
  0x00,
  0xc0,
  0x4f,
  0xd4,
  0x30,
  0xc8,
}};

static const xUuid g_ns_url = {{
  0x6b,
  0xa7,
  0xb8,
  0x11,
  0x9d,
  0xad,
  0x11,
  0xd1,
  0x80,
  0xb4,
  0x00,
  0xc0,
  0x4f,
  0xd4,
  0x30,
  0xc8,
}};

const xUuid *xUuidNamespaceDns(void) {
  return &g_ns_dns;
}
const xUuid *xUuidNamespaceUrl(void) {
  return &g_ns_url;
}
