/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * json_parse.c - Internal JSON Tokenizer (implementation)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <x/base/arena.h>
#include <x/json/json_parse.h>

/* ── Whitespace & Literals ──────────────────────────────────────────── */

void xjson_tok_skip_ws(const char **cur, const char *end) {
  while (*cur < end) {
    char c = **cur;
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    (*cur)++;
  }
}

int xjson_tok_match(const char **cur, const char *end, const char *lit) {
  size_t n = strlen(lit);
  xjson_tok_skip_ws(cur, end);
  if ((size_t)(end - *cur) < n) return 0;
  if (memcmp(*cur, lit, n) != 0) return 0;
  *cur += n;
  return 1;
}

/* ── String ─────────────────────────────────────────────────────────── */

int xjson_tok_string(const char **cur, const char *end, xArena *arena,
                      char **out, size_t *out_len) {
  const char *start;
  char       *dst, *buf;
  size_t      len;

  if (*cur >= end || **cur != '"') return -1;
  (*cur)++; /* skip opening '"' */

  start = *cur;

  /* First pass: find closing quote, detect escapes. */
  {
    int needs_unescape = 0;
    const char *s = *cur;
    while (s < end && *s != '"') {
      if (*s == '\\') { needs_unescape = 1; s++; if (s >= end) break; }
      s++;
    }
    if (s >= end) return -1;
    len = (size_t)(s - *cur);
    *cur = s + 1; /* skip closing '"' */

    if (!needs_unescape) {
      buf = (char *)xArenaAlloc(arena, len + 1);
      if (!buf) return -1;
      memcpy(buf, start, len);
      buf[len] = '\0';
      *out     = buf;
      *out_len = len;
      return 0;
    }
  }

  /* Slow path: unescape. */
  buf = (char *)xArenaAlloc(arena, len + 1);
  if (!buf) return -1;
  dst = buf;

  while (start < *cur) {
    char c = *start++;
    if (c == '"') break;
    if (c != '\\') { *dst++ = c; continue; }
    if (start >= *cur) break;
    c = *start++;
    switch (c) {
    case '"':  *dst++ = '"';  break;
    case '\\': *dst++ = '\\'; break;
    case '/':  *dst++ = '/';  break;
    case 'b':  *dst++ = '\b'; break;
    case 'f':  *dst++ = '\f'; break;
    case 'n':  *dst++ = '\n'; break;
    case 'r':  *dst++ = '\r'; break;
    case 't':  *dst++ = '\t'; break;
    case 'u': {
      unsigned codepoint = 0;
      int i;
      for (i = 0; i < 4; i++) {
        if (start >= *cur) return -1;
        char h = *start++;
        codepoint <<= 4;
        if      (h >= '0' && h <= '9') codepoint |= (unsigned)(h - '0');
        else if (h >= 'a' && h <= 'f') codepoint |= (unsigned)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') codepoint |= (unsigned)(h - 'A' + 10);
        else return -1;
      }
      if (codepoint <= 0x7F) {
        *dst++ = (char)codepoint;
      } else if (codepoint <= 0x7FF) {
        *dst++ = (char)(0xC0 | (codepoint >> 6));
        *dst++ = (char)(0x80 | (codepoint & 0x3F));
      } else {
        *dst++ = (char)(0xE0 |  (codepoint >> 12));
        *dst++ = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        *dst++ = (char)(0x80 |  (codepoint & 0x3F));
      }
      break;
    }
    default: return -1;
    }
  }

  *dst = '\0';
  *out     = buf;
  *out_len = (size_t)(dst - buf);
  return 0;
}

/* ── Number ─────────────────────────────────────────────────────────── */

int xjson_tok_number(const char **cur, const char *end,
                      int *is_double, int64_t *int_val, double *double_val) {
  const char *start = *cur;
  int has_dot_or_exp = 0;

  if (*cur < end && **cur == '-') (*cur)++;

  if (*cur >= end || (**cur < '0' || **cur > '9')) return -1;

  /* Reject leading zeros per RFC 8259 §6. */
  if (**cur == '0' && (*cur + 1) < end
      && (*cur)[1] >= '0' && (*cur)[1] <= '9') return -1;

  while (*cur < end && **cur >= '0' && **cur <= '9') (*cur)++;

  if (*cur < end && **cur == '.') {
    has_dot_or_exp = 1;
    (*cur)++;
    if (*cur >= end || **cur < '0' || **cur > '9') return -1;
    while (*cur < end && **cur >= '0' && **cur <= '9') (*cur)++;
  }

  if (*cur < end && (**cur == 'e' || **cur == 'E')) {
    has_dot_or_exp = 1;
    (*cur)++;
    if (*cur < end && (**cur == '+' || **cur == '-')) (*cur)++;
    if (*cur >= end || **cur < '0' || **cur > '9') return -1;
    while (*cur < end && **cur >= '0' && **cur <= '9') (*cur)++;
  }

  if (has_dot_or_exp) {
    *is_double  = 1;
    *double_val = strtod(start, NULL);
  } else {
    *is_double = 0;
    *int_val   = (int64_t)strtoll(start, NULL, 10);
  }
  return 0;
}
