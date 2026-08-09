/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * magnet.c - Magnet URI parser implementation
 */

#include <stdlib.h>
#include <string.h>

#include <xdl/magnet.h>

static int skip_prefix(const char **s, const char *prefix) {
  size_t n = strlen(prefix);
  if (strncmp(*s, prefix, n) != 0) return -1;
  *s += n;
  return 0;
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int hex_decode(const char *hex, uint8_t *out, size_t out_len) {
  for (size_t i = 0; i < out_len; i++) {
    int hi = hex_digit(hex[i * 2]);
    int lo = hex_digit(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 0;
}

static size_t url_decode(const char *src, size_t src_len, char *dst, size_t dst_cap) {
  size_t w = 0;
  for (size_t i = 0; i < src_len && w < dst_cap - 1; i++) {
    if (src[i] == '%' && i + 2 < src_len) {
      int hi = hex_digit(src[i + 1]);
      int lo = hex_digit(src[i + 2]);
      if (hi >= 0 && lo >= 0) {
        dst[w++] = (char)((hi << 4) | lo);
        i += 2;
        continue;
      }
    } else if (src[i] == '+') {
      dst[w++] = ' ';
      continue;
    }
    dst[w++] = src[i];
  }
  dst[w] = '\0';
  return w;
}

int xdl_magnet_parse(const char *uri, xdl_magnet_result_t *result) {
  if (!uri || !result) return -1;
  memset(result, 0, sizeof(*result));

  const char *p = uri;
  if (skip_prefix(&p, "magnet:?") < 0) return -1;

  int has_xt = 0;

  while (*p) {
    const char *kstart = p;
    while (*p && *p != '=' && *p != '&')
      p++;
    size_t klen = (size_t)(p - kstart);

    if (*p == '=') {
      p++;
      const char *vstart = p;
      while (*p && *p != '&')
        p++;
      size_t vlen = (size_t)(p - vstart);

      if (klen == 2 && strncmp(kstart, "xt", 2) == 0) {
        const char *v = vstart;
        if (skip_prefix(&v, "urn:btih:") < 0) return -1;
        if (vlen - (size_t)(v - vstart) != 40) return -1;
        if (hex_decode(v, result->info_hash, 20) < 0) return -1;
        has_xt = 1;
      } else if (klen == 2 && strncmp(kstart, "dn", 2) == 0) {
        url_decode(vstart, vlen, result->name, XDL_MAGNET_MAX_NAME);
      } else if (klen == 2 && strncmp(kstart, "tr", 2) == 0) {
        if (result->tracker_count >= XDL_MAGNET_MAX_TRACKERS) continue;
        char *t = (char *)malloc(vlen + 1);
        if (!t) return -1;
        url_decode(vstart, vlen, t, vlen + 1);
        result->trackers[result->tracker_count++] = t;
      }
    }

    if (*p == '&') p++;
  }

  if (!has_xt) return -1;
  return 0;
}

void xdl_magnet_result_free(xdl_magnet_result_t *result) {
  if (!result) return;
  for (int i = 0; i < result->tracker_count; i++) {
    free(result->trackers[i]);
    result->trackers[i] = NULL;
  }
  result->tracker_count = 0;
}
