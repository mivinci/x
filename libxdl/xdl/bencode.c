/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * bencode.c - BitTorrent bencoding parser/writer implementation
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xdl/bencode.h>

/* ── Constants ──────────────────────────────────────────── */

#define BENCODE_MAX_DEPTH  128
#define BENCODE_MAX_STRLEN (16 * 1024 * 1024)

/* ── Helpers ────────────────────────────────────────────── */

static int parse_decimal(const uint8_t *data, size_t len, int64_t *out) {
  if (len == 0 || !isdigit(*data)) return -1;
  char *end = NULL;
  *out      = (int64_t)strtoll((const char *)data, &end, 10);
  if (end == (const char *)data) return -1;
  return (int)(end - (const char *)data);
}

static int parse_value(const uint8_t *data, size_t len, size_t *pos, int depth,
                       xdl_bencode_value_t **out);

/* ── String parser ──────────────────────────────────────── */

static int parse_string(const uint8_t *data, size_t len, size_t *pos, xdl_bencode_value_t **out) {
  int64_t slen     = 0;
  int     consumed = parse_decimal(data + *pos, len - *pos, &slen);
  if (consumed < 0) return -1;
  *pos += consumed;

  if (*pos >= len || data[*pos] != ':') return -1;
  (*pos)++;

  if (slen < 0 || (size_t)slen > BENCODE_MAX_STRLEN) return -1;
  if (*pos + (size_t)slen > len) return -1;

  xdl_bencode_value_t *v = (xdl_bencode_value_t *)calloc(1, sizeof(*v));
  if (!v) return -1;
  v->type     = XDL_BENCODE_STRING;
  v->str.len  = (size_t)slen;
  v->str.data = (uint8_t *)malloc((size_t)slen + 1);
  if (!v->str.data) {
    free(v);
    return -1;
  }
  memcpy((void *)v->str.data, data + *pos, (size_t)slen);
  ((uint8_t *)v->str.data)[slen] = '\0';
  *pos += (size_t)slen;
  *out = v;
  return 0;
}

/* ── Integer parser ─────────────────────────────────────── */

static int parse_integer(const uint8_t *data, size_t len, size_t *pos, xdl_bencode_value_t **out) {
  if (*pos >= len || data[*pos] != 'i') return -1;
  (*pos)++;

  int sign = 1;
  if (*pos < len && data[*pos] == '-') {
    sign = -1;
    (*pos)++;
  }

  size_t digit_start = *pos;
  if (*pos >= len || !isdigit(data[*pos])) return -1;

  int64_t val      = 0;
  int     consumed = parse_decimal(data + *pos, len - *pos, &val);
  if (consumed < 0) return -1;
  *pos += consumed;

  if (data[digit_start] == '0') {
    if (consumed > 1) return -1;
    if (sign < 0) return -1;
  }

  if (*pos >= len || data[*pos] != 'e') return -1;
  (*pos)++;

  xdl_bencode_value_t *v = (xdl_bencode_value_t *)calloc(1, sizeof(*v));
  if (!v) return -1;
  v->type    = XDL_BENCODE_INTEGER;
  v->integer = val * sign;
  *out       = v;
  return 0;
}

/* ── List parser ────────────────────────────────────────── */

static int parse_list(const uint8_t *data, size_t len, size_t *pos, int depth,
                      xdl_bencode_value_t **out) {
  if (depth > BENCODE_MAX_DEPTH) return -1;
  if (*pos >= len || data[*pos] != 'l') return -1;
  (*pos)++;

  xdl_bencode_value_t *v = (xdl_bencode_value_t *)calloc(1, sizeof(*v));
  if (!v) return -1;
  v->type = XDL_BENCODE_LIST;

  size_t cap = 0;
  while (*pos < len && data[*pos] != 'e') {
    xdl_bencode_value_t *item = NULL;
    if (parse_value(data, len, pos, depth + 1, &item) < 0) {
      xdl_bencode_value_free(v);
      return -1;
    }
    if (v->list.count >= cap) {
      cap = cap ? cap * 2 : 8;
      xdl_bencode_value_t **new_items =
        (xdl_bencode_value_t **)realloc(v->list.items, cap * sizeof(xdl_bencode_value_t *));
      if (!new_items) {
        xdl_bencode_value_free(item);
        xdl_bencode_value_free(v);
        return -1;
      }
      v->list.items = new_items;
    }
    v->list.items[v->list.count++] = item;
  }

  if (*pos >= len || data[*pos] != 'e') {
    xdl_bencode_value_free(v);
    return -1;
  }
  (*pos)++;
  *out = v;
  return 0;
}

/* ── Dict parser ────────────────────────────────────────── */

static int parse_dict(const uint8_t *data, size_t len, size_t *pos, int depth,
                      xdl_bencode_value_t **out) {
  if (depth > BENCODE_MAX_DEPTH) return -1;
  if (*pos >= len || data[*pos] != 'd') return -1;
  (*pos)++;

  xdl_bencode_value_t *v = (xdl_bencode_value_t *)calloc(1, sizeof(*v));
  if (!v) return -1;
  v->type = XDL_BENCODE_DICT;

  size_t cap = 0;
  while (*pos < len && data[*pos] != 'e') {
    xdl_bencode_value_t *key = NULL;
    if (parse_string(data, len, pos, &key) < 0) {
      xdl_bencode_value_free(v);
      return -1;
    }

    xdl_bencode_value_t *val = NULL;
    if (parse_value(data, len, pos, depth + 1, &val) < 0) {
      xdl_bencode_value_free(key);
      xdl_bencode_value_free(v);
      return -1;
    }

    if (v->dict.count >= cap) {
      cap                = cap ? cap * 2 : 8;
      xdl_bencode_value_t **nk = (xdl_bencode_value_t **)realloc(v->dict.keys, cap * sizeof(xdl_bencode_value_t *));
      xdl_bencode_value_t **nv = (xdl_bencode_value_t **)realloc(v->dict.values, cap * sizeof(xdl_bencode_value_t *));
      if (!nk || !nv) {
        xdl_bencode_value_free(key);
        xdl_bencode_value_free(val);
        xdl_bencode_value_free(v);
        return -1;
      }
      v->dict.keys   = nk;
      v->dict.values = nv;
    }
    v->dict.keys[v->dict.count]   = key;
    v->dict.values[v->dict.count] = val;
    v->dict.count++;
  }

  if (*pos >= len || data[*pos] != 'e') {
    xdl_bencode_value_free(v);
    return -1;
  }
  (*pos)++;
  *out = v;
  return 0;
}

/* ── Top-level dispatch ─────────────────────────────────── */

static int parse_value(const uint8_t *data, size_t len, size_t *pos, int depth,
                       xdl_bencode_value_t **out) {
  if (*pos >= len) return -1;
  char c = (char)data[*pos];
  switch (c) {
  case 'i':
    return parse_integer(data, len, pos, out);
  case 'l':
    return parse_list(data, len, pos, depth, out);
  case 'd':
    return parse_dict(data, len, pos, depth, out);
  default:
    if (isdigit(c)) return parse_string(data, len, pos, out);
    return -1;
  }
}

/* ── Public: parse ──────────────────────────────────────── */

int xdl_bencode_parse(const uint8_t *data, size_t len, xdl_bencode_value_t **out) {
  if (!data || !len || !out) return -1;
  size_t pos = 0;
  int    ret = parse_value(data, len, &pos, 0, out);
  if (ret < 0) return -1;
  if (pos != len) {
    xdl_bencode_value_free(*out);
    *out = NULL;
    return -1;
  }
  return 0;
}

/* ── Write: measure ─────────────────────────────────────── */

static size_t measure_string(const xdl_bencode_value_t *v) {
  char lbuf[32];
  int  nd = snprintf(lbuf, sizeof(lbuf), "%zu", v->str.len);
  return (size_t)nd + 1 + v->str.len;
}

static size_t measure_integer(const xdl_bencode_value_t *v) {
  char ibuf[32];
  int  nd = snprintf(ibuf, sizeof(ibuf), "%" PRId64, v->integer);
  return 1 + (size_t)nd + 1;
}

static size_t measure_value(const xdl_bencode_value_t *v) {
  if (!v) return 0;
  switch (v->type) {
  case XDL_BENCODE_STRING:
    return measure_string(v);
  case XDL_BENCODE_INTEGER:
    return measure_integer(v);
  case XDL_BENCODE_LIST: {
    size_t n = 2;
    for (size_t i = 0; i < v->list.count; i++)
      n += measure_value(v->list.items[i]);
    return n;
  }
  case XDL_BENCODE_DICT: {
    size_t n = 2;
    for (size_t i = 0; i < v->dict.count; i++)
      n += measure_value(v->dict.keys[i]) + measure_value(v->dict.values[i]);
    return n;
  }
  }
  return 0;
}

/* ── Write: serialize ───────────────────────────────────── */

static int write_value(const xdl_bencode_value_t *v, uint8_t *buf, size_t *off) {
  if (!v) return -1;
  switch (v->type) {
  case XDL_BENCODE_STRING: {
    int nd = snprintf((char *)buf + *off, 32, "%zu:", v->str.len);
    if (nd < 0) return -1;
    *off += (size_t)nd;
    memcpy(buf + *off, v->str.data, v->str.len);
    *off += v->str.len;
    return 0;
  }
  case XDL_BENCODE_INTEGER: {
    buf[(*off)++] = 'i';
    int nd        = snprintf((char *)buf + *off, 32, "%" PRId64, v->integer);
    if (nd < 0) return -1;
    *off += (size_t)nd;
    buf[(*off)++] = 'e';
    return 0;
  }
  case XDL_BENCODE_LIST: {
    buf[(*off)++] = 'l';
    for (size_t i = 0; i < v->list.count; i++)
      if (write_value(v->list.items[i], buf, off) < 0) return -1;
    buf[(*off)++] = 'e';
    return 0;
  }
  case XDL_BENCODE_DICT: {
    buf[(*off)++] = 'd';
    for (size_t i = 0; i < v->dict.count; i++) {
      if (write_value(v->dict.keys[i], buf, off) < 0) return -1;
      if (write_value(v->dict.values[i], buf, off) < 0) return -1;
    }
    buf[(*off)++] = 'e';
    return 0;
  }
  }
  return -1;
}

int xdl_bencode_write(const xdl_bencode_value_t *value, uint8_t **out, size_t *out_len) {
  if (!value || !out || !out_len) return -1;
  size_t total = measure_value(value);
  *out         = (uint8_t *)malloc(total);
  if (!*out) return -1;
  *out_len   = total;
  size_t off = 0;
  if (write_value(value, *out, &off) < 0) {
    free(*out);
    *out     = NULL;
    *out_len = 0;
    return -1;
  }
  return 0;
}

/* ── Dict sort ─────────────────────────────────────────── */

static int cmp_bencode_string(const void *a, const void *b) {
  const xdl_bencode_value_t *va      = *(const xdl_bencode_value_t **)a;
  const xdl_bencode_value_t *vb      = *(const xdl_bencode_value_t **)b;
  size_t               min_len = va->str.len < vb->str.len ? va->str.len : vb->str.len;
  int                  cmp     = memcmp(va->str.data, vb->str.data, min_len);
  if (cmp != 0) return cmp;
  if (va->str.len < vb->str.len) return -1;
  if (va->str.len > vb->str.len) return 1;
  return 0;
}

void xdl_bencode_dict_sort(xdl_bencode_value_t *dict) {
  if (!dict || dict->type != XDL_BENCODE_DICT || dict->dict.count <= 1) return;
  for (size_t i = 0; i < dict->dict.count - 1; i++) {
    for (size_t j = i + 1; j < dict->dict.count; j++) {
      if (cmp_bencode_string(&dict->dict.keys[i], &dict->dict.keys[j]) > 0) {
        xdl_bencode_value_t *tk    = dict->dict.keys[i];
        xdl_bencode_value_t *tv    = dict->dict.values[i];
        dict->dict.keys[i]   = dict->dict.keys[j];
        dict->dict.values[i] = dict->dict.values[j];
        dict->dict.keys[j]   = tk;
        dict->dict.values[j] = tv;
      }
    }
  }
}

/* ─��� Free ───────────────────────────────────────────────── */

void xdl_bencode_value_free(xdl_bencode_value_t *value) {
  if (!value) return;
  switch (value->type) {
  case XDL_BENCODE_STRING:
    free((void *)value->str.data);
    break;
  case XDL_BENCODE_INTEGER:
    break;
  case XDL_BENCODE_LIST:
    for (size_t i = 0; i < value->list.count; i++)
      xdl_bencode_value_free(value->list.items[i]);
    free(value->list.items);
    break;
  case XDL_BENCODE_DICT:
    for (size_t i = 0; i < value->dict.count; i++) {
      xdl_bencode_value_free(value->dict.keys[i]);
      xdl_bencode_value_free(value->dict.values[i]);
    }
    free(value->dict.keys);
    free(value->dict.values);
    break;
  }
  free(value);
}

/* ── Dict find ─────────────────────────────────────────── */

xdl_bencode_value_t *xdl_bencode_dict_find(xdl_bencode_value_t *dict, const char *key) {
  if (!dict || dict->type != XDL_BENCODE_DICT || !key) return NULL;
  size_t klen = strlen(key);
  for (size_t i = 0; i < dict->dict.count; i++) {
    if (dict->dict.keys[i]->str.len == klen &&
        memcmp(dict->dict.keys[i]->str.data, key, klen) == 0) {
      return dict->dict.values[i];
    }
  }
  return NULL;
}

/* ── Info range ────────────────────────────────────────── */

int xdl_bencode_info_range(const uint8_t *data, size_t len, const uint8_t **start, size_t *slen) {
  if (!data || !len || !start || !slen) return -1;
  if (len < 2 || data[0] != 'd') return -1;

  size_t pos = 1;
  while (pos < len) {
    if (data[pos] == 'e') break;
    if (!isdigit(data[pos])) return -1;

    int64_t klen     = 0;
    int     consumed = parse_decimal(data + pos, len - pos, &klen);
    if (consumed < 0 || klen < 0) return -1;
    pos += consumed;
    if (pos >= len || data[pos] != ':') return -1;
    pos++;

    if (pos + (size_t)klen > len) return -1;
    int is_info = (klen == 4 && memcmp(data + pos, "info", 4) == 0);
    pos += (size_t)klen;

    const uint8_t *val_start = data + pos;
    size_t         val_pos   = pos;

    if (pos >= len) return -1;
    char c = (char)data[pos];
    if (c == 'i') {
      pos++;
      while (pos < len && data[pos] != 'e')
        pos++;
      if (pos < len) pos++;
    } else if (isdigit(c)) {
      int64_t slen2 = 0;
      consumed      = parse_decimal(data + pos, len - pos, &slen2);
      if (consumed < 0) return -1;
      pos += consumed;
      if (pos >= len || data[pos] != ':') return -1;
      pos++;
      if ((size_t)slen2 > len - pos) return -1;
      pos += (size_t)slen2;
    } else if (c == 'l' || c == 'd') {
      int depth = 1;
      pos++;
      while (pos < len && depth > 0) {
        if (data[pos] == 'e') {
          depth--;
          pos++;
        } else if (data[pos] == 'l' || data[pos] == 'd') {
          depth++;
          pos++;
        } else if (isdigit(data[pos])) {
          int64_t slen2 = 0;
          consumed      = parse_decimal(data + pos, len - pos, &slen2);
          if (consumed < 0) return -1;
          pos += consumed;
          if (pos >= len || data[pos] != ':') return -1;
          pos++;
          if ((size_t)slen2 > len - pos) return -1;
          pos += (size_t)slen2;
        } else if (data[pos] == 'i') {
          pos++;
          while (pos < len && data[pos] != 'e')
            pos++;
          if (pos < len) pos++;
        } else {
          return -1;
        }
      }
    } else {
      return -1;
    }

    if (is_info) {
      *start = val_start;
      *slen  = pos - val_pos;
      return 0;
    }
  }

  return -1;
}
