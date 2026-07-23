/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * json_sax.c - SAX (Streaming) JSON Parser (implementation)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <x/base/arena.h>
#include <x/json/json_parse.h>
#include <x/json/json_sax.h>

/* ── Internal state ──────────────────────────────────────────────────── */

struct xJsonSax_ {
  const xJsonSaxHandler *handler;
  void                  *ctx;
  int                    max_depth;

  /* Temporary arena for decoded strings (one-shot and streaming). */
  xArena                *arena;

  /* Parser position (reset per Feed call). */
  const char            *cur;
  const char            *end;
  int                    error;

  /* Depth tracking for nesting checks. */
  int                    depth;
};

/* ── One-shot SAX ────────────────────────────────────────────────────── */

/* Forward declarations for recursive descent. */
static int xjson_sax_value_(struct xJsonSax_ *s);

static int xjson_sax_string_(struct xJsonSax_ *s) {
  char *str;
  size_t len;

  if (xjson_tok_string(&s->cur, s->end, s->arena, &str, &len) != 0) {
    s->error = 1; return -1;
  }
  return s->handler->on_string(s->ctx, str, len);
}

static int xjson_sax_array_(struct xJsonSax_ *s) {
  int r;

  s->cur++; /* skip '[' */
  s->depth++;

  r = s->handler->on_array_begin(s->ctx);
  if (r != 0) return r;

  if (xjson_tok_match(&s->cur, s->end, "]")) {
    r = s->handler->on_array_end(s->ctx);
    s->depth--;
    return r;
  }

  for (;;) {
    r = xjson_sax_value_(s);
    if (r != 0) return (r < 0) ? -1 : r;

    if (xjson_tok_match(&s->cur, s->end, "]")) break;
    if (!xjson_tok_match(&s->cur, s->end, ",")) { s->error = 1; return -1; }
  }

  s->depth--;
  return s->handler->on_array_end(s->ctx);
}

static int xjson_sax_object_(struct xJsonSax_ *s) {
  int r;
  char *key;
  size_t key_len;

  s->cur++; /* skip '{' */
  s->depth++;

  r = s->handler->on_object_begin(s->ctx);
  if (r != 0) return r;

  if (xjson_tok_match(&s->cur, s->end, "}")) {
    r = s->handler->on_object_end(s->ctx);
    s->depth--;
    return r;
  }

  for (;;) {
    if (xjson_tok_string(&s->cur, s->end, s->arena, &key, &key_len) != 0) {
      s->error = 1; return -1;
    }
    if (!xjson_tok_match(&s->cur, s->end, ":")) { s->error = 1; return -1; }

    r = s->handler->on_key(s->ctx, key, key_len);
    if (r != 0) return r;

    r = xjson_sax_value_(s);
    if (r != 0) return (r < 0) ? -1 : r;

    if (xjson_tok_match(&s->cur, s->end, "}")) break;
    if (!xjson_tok_match(&s->cur, s->end, ",")) { s->error = 1; return -1; }
  }

  s->depth--;
  return s->handler->on_object_end(s->ctx);
}

static int xjson_sax_value_(struct xJsonSax_ *s) {
  int is_double;
  int64_t int_val;
  double double_val;

  xjson_tok_skip_ws(&s->cur, s->end);
  if (s->cur >= s->end) { s->error = 1; return -1; }

  switch (*s->cur) {
  case '"': return xjson_sax_string_(s);
  case '{': return xjson_sax_object_(s);
  case '[': return xjson_sax_array_(s);
  case 't':
    if (xjson_tok_match(&s->cur, s->end, "true")) return s->handler->on_bool(s->ctx, 1);
    break;
  case 'f':
    if (xjson_tok_match(&s->cur, s->end, "false")) return s->handler->on_bool(s->ctx, 0);
    break;
  case 'n':
    if (xjson_tok_match(&s->cur, s->end, "null")) return s->handler->on_null(s->ctx);
    break;
  default:
    if (*s->cur == '-' || (*s->cur >= '0' && *s->cur <= '9')) {
      if (xjson_tok_number(&s->cur, s->end, &is_double, &int_val, &double_val) != 0) {
        s->error = 1; return -1;
      }
      if (is_double) return s->handler->on_double(s->ctx, double_val);
      else return s->handler->on_int(s->ctx, int_val);
    }
    break;
  }

  s->error = 1;
  return -1;
}

int xJsonSaxParse(const char *json, size_t len,
                   const xJsonSaxHandler *handler, void *ctx) {
  struct xJsonSax_ s;
  int r;

  if (!json || len == 0 || !handler) return -1;

  s.handler = handler;
  s.ctx     = ctx;
  s.arena   = xArenaCreate(4096);
  if (!s.arena) return -1;

  s.cur     = json;
  s.end     = json + len;
  s.error   = 0;
  s.depth   = 0;

  r = xjson_sax_value_(&s);

  /* Check for trailing garbage */
  if (r == 0) {
    xjson_tok_skip_ws(&s.cur, s.end);
    if (s.cur < s.end) r = -1;
  }

  xArenaDestroy(s.arena);
  return r;
}

/* ── Streaming SAX (stubs) ────────────────────────────────────────────── */

xJsonSax *xJsonSaxCreate(const xJsonSaxHandler *handler, void *ctx, int max_depth) {
  struct xJsonSax_ *s;

  if (!handler) return NULL;
  if (max_depth <= 0) max_depth = 32;

  s = (struct xJsonSax_ *)calloc(1, sizeof(*s));
  if (!s) return NULL;

  s->handler   = handler;
  s->ctx       = ctx;
  s->max_depth = max_depth;
  s->arena     = xArenaCreate(4096);
  if (!s->arena) { free(s); return NULL; }

  return (xJsonSax *)s;
}

/* TODO: Implement incremental feed with an explicit state machine that
 * can pause/resume across buffer boundaries.  The current one-shot
 * recursive descent parser cannot be used for streaming because it would
 * lose state when a partial token spans a chunk boundary. */
xJsonSaxResult xJsonSaxFeed(xJsonSax *sax, const char *data, size_t len) {
  (void)sax; (void)data; (void)len;
  return xJsonSaxResult_Error; /* not yet implemented */
}

xJsonSaxResult xJsonSaxFinalize(xJsonSax *sax) {
  (void)sax;
  return xJsonSaxResult_Error; /* not yet implemented */
}

void xJsonSaxReset(xJsonSax *sax) {
  struct xJsonSax_ *s = (struct xJsonSax_ *)sax;
  if (!s) return;

  xArenaReset(s->arena);
  s->cur     = NULL;
  s->end     = NULL;
  s->error   = 0;
  s->depth   = 0;
}

void xJsonSaxDestroy(xJsonSax *sax) {
  struct xJsonSax_ *s = (struct xJsonSax_ *)sax;
  if (!s) return;

  if (s->arena) xArenaDestroy(s->arena);
  free(s);
}
