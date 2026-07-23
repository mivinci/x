/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * json.c - DOM-Style JSON Parser / Builder / Serializer (implementation)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <x/base/arena.h>
#include <x/json/json.h>
#include <x/json/json_parse.h>

/* ── Internal structures ─────────────────────────────────────────────── */

struct xJson_ {
  uint8_t            flags;    /* type (bit 0-3) + owned flag (bit 4) */
  uint8_t            pad[7];
  xArena            *arena;    /* NULL = malloc mode; non-NULL = arena mode */

  /* Object-member key (NULL for array elements and standalone nodes). */
  char              *key;
  size_t             key_len;

  /* Tagged union for the value.  For OBJECT / ARRAY, the pointer is the
   * first child in a singly-linked sibling list threaded through ->next. */
  union {
    int              bool_val;
    int64_t          int_val;
    double           double_val;
    struct {
      char          *ptr;
      size_t         len;
    } str;
    struct xJson_   *object;   /* XJSON_OBJECT: first key-value pair child */
    struct xJson_   *array;    /* XJSON_ARRAY:  first element child        */
  };

  /* Singly-linked sibling list (children of an object or array). */
  struct xJson_     *next;
};

struct xJsonIterator_ {
  uint8_t            flags;    /* XJSON_ITERATOR */
  uint8_t            pad[7];
  const struct xJson_ *obj;    /* object being iterated */
  struct xJson_     *cur;      /* current child, NULL before first Next() */
};

#define XJSON_(p)      ((struct xJson_ *)(p))
#define XJSON_C(p)     ((const struct xJson_ *)(p))
#define XJSON_ITER(p)  ((struct xJsonIterator_ *)(p))

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void xjson_strcpy_(char *dst, const char *src, size_t len) {
  memcpy(dst, src, len);
  dst[len] = '\0';
}

/* Free one malloc-backed node and its subtree (used only in malloc mode). */
static void xjson_free_subtree_(struct xJson_ *node) {
  struct xJson_ *child, *next;

  if (!node) return;

  /* Recursively free children. */
  {
    uint8_t type = node->flags & XJSON_TYPE_MASK;
    child = (type == XJSON_OBJECT) ? node->object : (type == XJSON_ARRAY) ? node->array : NULL;
  }
  while (child) {
    next = child->next;
    xjson_free_subtree_(child);
    child = next;
  }

  free(node->key);
  /* Free string copy only in malloc mode. */
  if (!node->arena && (node->flags & XJSON_TYPE_MASK) == XJSON_STRING)
    free(node->str.ptr);

  free(node);
}

/* ── Free dispatcher ─────────────────────────────────────────────────── */

void xJsonFree(void *ptr) {
  uint8_t type;
  struct xJson_ *node;
  struct xJsonIterator_ *it;

  if (!ptr) return;

  type = *(uint8_t *)ptr & XJSON_TYPE_MASK;

  if (type == XJSON_ITERATOR) {
    it = XJSON_ITER(ptr);
    free(it);
    return;
  }

  node = XJSON_(ptr);
  if (node->flags & XJSON_FLAG_OWNED) return;

  if (node->arena) {
    xArenaDestroy(node->arena);           /* O(1) — entire parse tree */
  } else {
    xjson_free_subtree_(node);            /* recursive free */
  }
}

/* ── Construct (malloc mode) ─────────────────────────────────────────── */

static struct xJson_ *xjson_new_(uint8_t type) {
  struct xJson_ *node;

  node = (struct xJson_ *)calloc(1, sizeof(*node));
  if (!node) return NULL;

  node->flags = type;
  node->arena = NULL;
  return node;
}

xJson *xJsonNewNull(void)           { return (xJson *)xjson_new_(XJSON_NULL); }
xJson *xJsonNewBool(int v)          { struct xJson_ *n = xjson_new_(XJSON_BOOL);  if (n) n->bool_val  = v ? 1 : 0; return (xJson *)n; }
xJson *xJsonNewInt(int64_t v)       { struct xJson_ *n = xjson_new_(XJSON_INT);   if (n) n->int_val   = v; return (xJson *)n; }
xJson *xJsonNewDouble(double v)     { struct xJson_ *n = xjson_new_(XJSON_DOUBLE); if (n) n->double_val = v; return (xJson *)n; }

xJson *xJsonNewString(const char *str) {
  return xJsonNewStringN(str, str ? strlen(str) : 0);
}

xJson *xJsonNewStringN(const char *str, size_t len) {
  struct xJson_ *node;
  char          *copy;

  node = xjson_new_(XJSON_STRING);
  if (!node) return NULL;

  copy = (char *)malloc(len + 1);
  if (!copy) { free(node); return NULL; }

  node->str.ptr = copy;
  node->str.len = len;
  xjson_strcpy_(copy, str ? str : "", len);
  return (xJson *)node;
}

xJson *xJsonNewArray(void)  { return (xJson *)xjson_new_(XJSON_ARRAY); }
xJson *xJsonNewObject(void) { return (xJson *)xjson_new_(XJSON_OBJECT); }

/* ── Query ───────────────────────────────────────────────────────────── */

int          xJsonType(const xJson *node)   { return XJSON_C(node)->flags & XJSON_TYPE_MASK; }
int          xJsonBool(const xJson *node)    { return XJSON_C(node)->bool_val; }
int64_t      xJsonInt(const xJson *node)     { return XJSON_C(node)->int_val; }
double       xJsonDouble(const xJson *node)  { return XJSON_C(node)->double_val; }
const char  *xJsonString(const xJson *node)  { return XJSON_C(node)->str.ptr; }
size_t       xJsonStringLength(const xJson *node)  { return XJSON_C(node)->str.len; }

/* ── Object ──────────────────────────────────────────────────────────── */

/* Return a pointer to the ->next field of the child whose key matches,
 * or to the last ->next (NULL) if not found.  Caller uses this pointer
 * for link/unlink without needing a prev pointer. */
static struct xJson_ **xjson_obj_find_(struct xJson_ *obj, const char *key) {
  struct xJson_ **pp;
  for (pp = &obj->object; *pp; pp = &(*pp)->next)
    if ((*pp)->key && strcmp((*pp)->key, key) == 0) return pp;
  return pp; /* points to tail ->next (NULL) if not found */
}

xJson *xJsonObjectGet(const xJson *obj, const char *key) {
  struct xJson_ *child;

  if (!obj || !key) return NULL;

  for (child = XJSON_C(obj)->object; child; child = child->next)
    if (child->key && strcmp(child->key, key) == 0) return (xJson *)child;
  return NULL;
}

int xJsonObjectSet(xJson *obj, const char *key, xJson *val) {
  struct xJson_  *o = XJSON_(obj);
  struct xJson_  *v = XJSON_(val);
  struct xJson_ **pp;

  if (!o || (o->flags & XJSON_TYPE_MASK) != XJSON_OBJECT || !v) return -1;

  v->flags |= XJSON_FLAG_OWNED;

  free(v->key);
  {
    size_t klen = strlen(key) + 1;
    v->key = (char *)malloc(klen);
    if (v->key) memcpy(v->key, key, klen);
    v->key_len = klen - 1;   /* strlen(key) without re-scanning */
  }

  pp = xjson_obj_find_(o, key);
  if (*pp) {
    /* Replace existing. */
    struct xJson_ *old = *pp;
    v->next = old->next;
    *pp     = v;
    old->next = NULL;      /* detach before free */
    xjson_free_subtree_(old);
  } else {
    /* Append.  pp points to the last child's ->next (NULL). */
    *pp    = v;
    v->next = NULL;
  }

  return 0;
}

void xJsonObjectDel(xJson *obj, const char *key) {
  struct xJson_  *o = XJSON_(obj);
  struct xJson_ **pp;

  if (!o || (o->flags & XJSON_TYPE_MASK) != XJSON_OBJECT || !key) return;

  pp = xjson_obj_find_(o, key);
  if (!*pp) return;

  struct xJson_ *victim = *pp;
  *pp = victim->next;         /* unlink in one instruction */
  victim->next = NULL;
  xjson_free_subtree_(victim);
}

int xJsonObjectSize(const xJson *obj) {
  int count = 0;
  const struct xJson_ *child;
  for (child = XJSON_C(obj)->object; child; child = child->next) count++;
  return count;
}

/* ── Object Iterator ─────────────────────────────────────────────────── */

xJsonIterator *xJsonNewIterator(const xJson *obj) {
  struct xJsonIterator_ *it;

  if (!obj || (XJSON_C(obj)->flags & XJSON_TYPE_MASK) != XJSON_OBJECT)
    return NULL;

  it = (struct xJsonIterator_ *)calloc(1, sizeof(*it));
  if (!it) return NULL;

  it->flags = XJSON_ITERATOR;
  it->obj   = XJSON_C(obj);
  it->cur   = NULL;
  return (xJsonIterator *)it;
}

int xJsonIteratorNext(xJsonIterator *it) {
  struct xJsonIterator_ *iter = XJSON_ITER(it);
  if (!iter) return 0;
  iter->cur = iter->cur ? iter->cur->next : iter->obj->object;
  return iter->cur != NULL;
}

const char *xJsonIteratorKey(xJsonIterator *it, size_t *len) {
  struct xJsonIterator_ *iter = XJSON_ITER(it);
  if (!iter || !iter->cur) return NULL;
  if (len) *len = iter->cur->key_len;
  return iter->cur->key;
}

xJson *xJsonIteratorValue(xJsonIterator *it) {
  struct xJsonIterator_ *iter = XJSON_ITER(it);
  if (!iter || !iter->cur) return NULL;
  return (xJson *)iter->cur;
}

/* ── Array ────────────────────────────────────────────────────────────── */

/* Walk to the idx-th element (0-based).  Negative indices from end. */
static struct xJson_ *xjson_arr_nth_(struct xJson_ *arr, int idx) {
  int n = 0;
  struct xJson_ *p;

  if (idx < 0) {
    int total = 0;
    for (p = arr->array; p; p = p->next) total++;
    idx += total;
    if (idx < 0) return NULL;
  }

  for (p = arr->array; p && n < idx; p = p->next) n++;
  return (n == idx) ? p : NULL;
}

/* Return a pointer to the ->next field of the element BEFORE position idx
 * (or to arr->array if idx == 0).  Returns NULL on out-of-bounds. */
static struct xJson_ **xjson_arr_ptr_at_(struct xJson_ *arr, int idx) {
  struct xJson_ **pp;
  int n = 0;

  if (idx < 0) {
    int total = 0;
    struct xJson_ *p;
    for (p = arr->array; p; p = p->next) total++;
    idx += total;
    if (idx < 0) return NULL;
  }

  for (pp = &arr->array; *pp && n < idx; pp = &(*pp)->next) n++;
  if (n != idx) return NULL;
  return pp;
}

xJson *xJsonArrayGet(const xJson *arr, int idx) {
  struct xJson_ *a = (struct xJson_ *)arr; /* const-cast: read-only walk */
  struct xJson_ *node;

  if (!a || (a->flags & XJSON_TYPE_MASK) != XJSON_ARRAY) return NULL;
  node = xjson_arr_nth_(a, idx);
  return node ? (xJson *)node : NULL;
}

int xJsonArraySet(xJson *arr, int idx, xJson *val) {
  struct xJson_  *a = XJSON_(arr);
  struct xJson_  *v = XJSON_(val);
  struct xJson_ **pp;

  if (!a || (a->flags & XJSON_TYPE_MASK) != XJSON_ARRAY || !v) return -1;

  pp = xjson_arr_ptr_at_(a, idx);
  if (!pp || !*pp) return -1;

  v->flags |= XJSON_FLAG_OWNED;
  v->next = (*pp)->next;
  {
    struct xJson_ *old = *pp;
    *pp      = v;
    old->next = NULL;
    xjson_free_subtree_(old);
  }
  return 0;
}

int xJsonArrayAppend(xJson *arr, xJson *val) {
  struct xJson_  *a = XJSON_(arr);
  struct xJson_  *v = XJSON_(val);
  struct xJson_ **pp;
  int count = 0;

  if (!a || (a->flags & XJSON_TYPE_MASK) != XJSON_ARRAY || !v) return -1;

  v->flags |= XJSON_FLAG_OWNED;
  v->next = NULL;

  /* Walk to end of list. */
  for (pp = &a->array; *pp; pp = &(*pp)->next) count++;
  *pp = v;
  return 0;
}

int xJsonArrayInsert(xJson *arr, int idx, xJson *val) {
  struct xJson_  *a = XJSON_(arr);
  struct xJson_  *v = XJSON_(val);
  struct xJson_ **pp;

  if (!a || (a->flags & XJSON_TYPE_MASK) != XJSON_ARRAY || !v) return -1;

  pp = xjson_arr_ptr_at_(a, idx);
  if (!pp) return -1;

  v->flags |= XJSON_FLAG_OWNED;
  v->next = *pp;
  *pp     = v;
  return 0;
}

void xJsonArrayRemove(xJson *arr, int idx) {
  struct xJson_  *a = XJSON_(arr);
  struct xJson_ **pp;

  if (!a || (a->flags & XJSON_TYPE_MASK) != XJSON_ARRAY) return;

  pp = xjson_arr_ptr_at_(a, idx);
  if (!pp || !*pp) return;

  struct xJson_ *victim = *pp;
  *pp = victim->next;
  victim->next = NULL;
  xjson_free_subtree_(victim);
}

int xJsonArraySize(const xJson *arr) {
  int count = 0;
  const struct xJson_ *p;
  for (p = XJSON_C(arr)->array; p; p = p->next) count++;
  return count;
}

/* ── Parse ───────────────────────────────────────────────────────────── */

typedef struct {
  const char *cur;
  const char *end;
  xArena     *arena;
  int         copy_strings;   /* 1 = copy, 0 = zero-copy (reference input) */
  int         error;
} xjson_parser_t;

static void xjson_p_skip_ws_(xjson_parser_t *p) {
  xjson_tok_skip_ws(&p->cur, p->end);
}

static int xjson_p_eof_(xjson_parser_t *p) {
  xjson_p_skip_ws_(p);
  return p->cur >= p->end;
}

static int xjson_p_match_(xjson_parser_t *p, const char *lit) {
  return xjson_tok_match(&p->cur, p->end, lit);
}

static void *xjson_p_alloc_(xjson_parser_t *p, size_t size) {
  void *ptr = xArenaAlloc(p->arena, size);
  if (!ptr) p->error = 1;
  return ptr;
}

static struct xJson_ *xjson_p_new_node_(xjson_parser_t *p, uint8_t type) {
  struct xJson_ *node;
  node = (struct xJson_ *)xjson_p_alloc_(p, sizeof(*node));
  if (!node) return NULL;
  memset(node, 0, sizeof(*node));
  node->flags = type;
  node->arena = p->arena;
  return node;
}

/* Forward. */
static struct xJson_ *xjson_p_value_(xjson_parser_t *p);

static struct xJson_ *xjson_p_string_node_(xjson_parser_t *p) {
  char   *str;
  size_t  len;

  if (xjson_tok_string(&p->cur, p->end, p->arena, &str, &len) != 0) {
    p->error = 1; return NULL;
  }

  struct xJson_ *node = xjson_p_new_node_(p, XJSON_STRING);
  if (!node) return NULL;
  node->str.ptr = str;
  node->str.len = len;
  return node;
}

static struct xJson_ *xjson_p_number_(xjson_parser_t *p) {
  int is_double;
  int64_t int_val;
  double double_val;

  if (xjson_tok_number(&p->cur, p->end, &is_double, &int_val, &double_val) != 0) {
    p->error = 1; return NULL;
  }

  if (is_double) {
    struct xJson_ *node = xjson_p_new_node_(p, XJSON_DOUBLE);
    if (!node) return NULL;
    node->double_val = double_val;
    return node;
  } else {
    struct xJson_ *node = xjson_p_new_node_(p, XJSON_INT);
    if (!node) return NULL;
    node->int_val = int_val;
    return node;
  }
}

static struct xJson_ *xjson_p_array_(xjson_parser_t *p) {
  struct xJson_  *arr  = xjson_p_new_node_(p, XJSON_ARRAY);
  struct xJson_ **tail = &arr->array;   /* pointer to last ->next */

  if (!arr) return NULL;

  p->cur++; /* skip '[' */

  if (xjson_p_match_(p, "]")) return arr;

  for (;;) {
    struct xJson_ *elem = xjson_p_value_(p);
    if (!elem) { p->error = 1; return NULL; }

    elem->flags |= XJSON_FLAG_OWNED;
    *tail = elem;
    tail  = &elem->next;

    if (xjson_p_match_(p, "]")) break;
    if (!xjson_p_match_(p, ",")) { p->error = 1; return NULL; }
  }

  return arr;
}

static struct xJson_ *xjson_p_object_(xjson_parser_t *p) {
  struct xJson_  *obj  = xjson_p_new_node_(p, XJSON_OBJECT);
  struct xJson_ **tail = &obj->object;

  if (!obj) return NULL;

  p->cur++; /* skip '{' */

  if (xjson_p_match_(p, "}")) return obj;

  for (;;) {
    char   *key;
    size_t  key_len;

    if (xjson_tok_string(&p->cur, p->end, p->arena, &key, &key_len) != 0) { p->error = 1; return NULL; }
    if (!xjson_p_match_(p, ":")) { p->error = 1; return NULL; }

    struct xJson_ *val = xjson_p_value_(p);
    if (!val) { p->error = 1; return NULL; }

    val->flags  |= XJSON_FLAG_OWNED;
    val->key     = key;
    val->key_len = key_len;
    *tail = val;
    tail  = &val->next;

    if (xjson_p_match_(p, "}")) break;
    if (!xjson_p_match_(p, ",")) { p->error = 1; return NULL; }
  }

  return obj;
}

static struct xJson_ *xjson_p_value_(xjson_parser_t *p) {
  xjson_p_skip_ws_(p);
  if (p->cur >= p->end) { p->error = 1; return NULL; }

  switch (*p->cur) {
  case '"': return xjson_p_string_node_(p);
  case '{': return xjson_p_object_(p);
  case '[': return xjson_p_array_(p);
  case 't':
    if (xjson_p_match_(p, "true")) {
      struct xJson_ *n = xjson_p_new_node_(p, XJSON_BOOL);
      if (n) n->bool_val = 1;
      return n;
    }
    break;
  case 'f':
    if (xjson_p_match_(p, "false")) {
      struct xJson_ *n = xjson_p_new_node_(p, XJSON_BOOL);
      if (n) n->bool_val = 0;
      return n;
    }
    break;
  case 'n':
    if (xjson_p_match_(p, "null"))
      return xjson_p_new_node_(p, XJSON_NULL);
    break;
  default:
    if (*p->cur == '-' || (*p->cur >= '0' && *p->cur <= '9'))
      return xjson_p_number_(p);
    break;
  }

  p->error = 1;
  return NULL;
}

/* Parse entry point. */
static xJson *xjson_do_parse_(const char *json, size_t len, int copy_strings) {
  xjson_parser_t parser;
  struct xJson_ *root;
  size_t arena_cap;

  if (!json || len == 0) return NULL;

  /* Heuristic: input_size * 8. */
  arena_cap = len * 8;
  if (arena_cap < 4096) arena_cap = 4096;

  parser.arena = xArenaCreate(arena_cap);
  if (!parser.arena) return NULL;

  parser.cur          = json;
  parser.end          = json + len;
  parser.copy_strings = copy_strings;
  parser.error        = 0;

  root = xjson_p_value_(&parser);

  if (!parser.error && !xjson_p_eof_(&parser)) parser.error = 1;

  if (parser.error || !root) {
    xArenaDestroy(parser.arena);
    return NULL;
  }

  root->arena = parser.arena;
  return (xJson *)root;
}

xJson *xJsonParse(const char *json, size_t len)     { return xjson_do_parse_(json, len, 0); }
xJson *xJsonParseCopy(const char *json, size_t len) { return xjson_do_parse_(json, len, 1); }

/* ── Serialise ───────────────────────────────────────────────────────── */

static int xjson_strify_impl_(const struct xJson_ *node, int pretty,
                               char *buf, size_t cap, size_t *pos,
                               int indent);

/* Write raw bytes; always advances *pos.  Returns 0 on success, -1 if buf is
 * non-NULL and the write would overflow cap.  buf==NULL means "measure only". */
static int xjson_write_(char *buf, size_t cap, size_t *pos,
                         const char *str, size_t len) {
  *pos += len;
  if (buf && *pos <= cap) { memcpy(buf + *pos - len, str, len); return 0; }
  return buf ? -1 : 0;
}

static int xjson_write_c_(char *buf, size_t cap, size_t *pos, char c) {
  return xjson_write_(buf, cap, pos, &c, 1);
}

static int xjson_write_indent_(char *buf, size_t cap, size_t *pos, int depth) {
  int r = 0;
  r |= xjson_write_c_(buf, cap, pos, '\n');
  for (int i = 0; i < depth * 2; i++) r |= xjson_write_c_(buf, cap, pos, ' ');
  return r;
}

static int xjson_stringify_str_(const char *str, size_t len,
                                 char *buf, size_t cap, size_t *pos) {
  int r = 0;
  r |= xjson_write_c_(buf, cap, pos, '"');
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    switch (c) {
    case '"':  r |= xjson_write_(buf, cap, pos, "\\\"", 2); break;
    case '\\': r |= xjson_write_(buf, cap, pos, "\\\\", 2); break;
    case '\b': r |= xjson_write_(buf, cap, pos, "\\b",  2); break;
    case '\f': r |= xjson_write_(buf, cap, pos, "\\f",  2); break;
    case '\n': r |= xjson_write_(buf, cap, pos, "\\n",  2); break;
    case '\r': r |= xjson_write_(buf, cap, pos, "\\r",  2); break;
    case '\t': r |= xjson_write_(buf, cap, pos, "\\t",  2); break;
    default:
      if (c < 0x20) {
        char esc[7];
        snprintf(esc, sizeof(esc), "\\u%04x", c);
        r |= xjson_write_(buf, cap, pos, esc, 6);
      } else {
        r |= xjson_write_c_(buf, cap, pos, (char)c);
      }
      break;
    }
  }
  r |= xjson_write_c_(buf, cap, pos, '"');
  return r;
}

static int xjson_strify_impl_(const struct xJson_ *node, int pretty,
                               char *buf, size_t cap, size_t *pos,
                               int indent) {
  int r = 0;
  char tmp[64];
  int tmp_len;

  if (!node) return 0;

  switch (node->flags & XJSON_TYPE_MASK) {
  case XJSON_NULL:
    r |= xjson_write_(buf, cap, pos, "null", 4);
    break;

  case XJSON_BOOL:
    r |= xjson_write_(buf, cap, pos, node->bool_val ? "true" : "false",
                      node->bool_val ? 4 : 5);
    break;

  case XJSON_INT:
    tmp_len = snprintf(tmp, sizeof(tmp), "%lld", (long long)node->int_val);
    r |= xjson_write_(buf, cap, pos, tmp, (size_t)tmp_len);
    break;

  case XJSON_DOUBLE: {
    tmp_len = snprintf(tmp, sizeof(tmp), "%.17g", node->double_val);
    { int has_dot = 0;
      for (int i = 0; i < tmp_len; i++)
        if (tmp[i] == '.' || tmp[i] == 'e' || tmp[i] == 'E') { has_dot = 1; break; }
      if (!has_dot) { tmp[tmp_len++] = '.'; tmp[tmp_len++] = '0'; } }
    r |= xjson_write_(buf, cap, pos, tmp, (size_t)tmp_len);
    break;
  }

  case XJSON_STRING:
    r |= xjson_stringify_str_(node->str.ptr, node->str.len, buf, cap, pos);
    break;

  case XJSON_ARRAY: {
    const struct xJson_ *child = node->array;
    r |= xjson_write_c_(buf, cap, pos, '[');
    if (!child) { r |= xjson_write_c_(buf, cap, pos, ']'); break; }
    while (child) {
      if (child != node->array) r |= xjson_write_c_(buf, cap, pos, ',');
      if (pretty) r |= xjson_write_indent_(buf, cap, pos, indent + 1);
      r |= xjson_strify_impl_(child, pretty, buf, cap, pos, indent + 1);
      child = child->next;
    }
    if (pretty) r |= xjson_write_indent_(buf, cap, pos, indent);
    r |= xjson_write_c_(buf, cap, pos, ']');
    break;
  }

  case XJSON_OBJECT: {
    const struct xJson_ *child = node->object;
    r |= xjson_write_c_(buf, cap, pos, '{');
    if (!child) { r |= xjson_write_c_(buf, cap, pos, '}'); break; }
    while (child) {
      if (child != node->object) r |= xjson_write_c_(buf, cap, pos, ',');
      if (pretty) r |= xjson_write_indent_(buf, cap, pos, indent + 1);
      r |= xjson_stringify_str_(child->key, child->key_len, buf, cap, pos);
      r |= xjson_write_c_(buf, cap, pos, ':');
      if (pretty) r |= xjson_write_c_(buf, cap, pos, ' ');
      r |= xjson_strify_impl_(child, pretty, buf, cap, pos, indent + 1);
      child = child->next;
    }
    if (pretty) r |= xjson_write_indent_(buf, cap, pos, indent);
    r |= xjson_write_c_(buf, cap, pos, '}');
    break;
  }

  default: break;
  }

  return r;
}

static char *xjson_stringify_alloc_(const xJson *node, int pretty) {
  size_t pos = 0;
  int r;

  if (!node) return NULL;

  r = xjson_strify_impl_(XJSON_C(node), pretty, NULL, 0, &pos, 0);
  if (r != 0) return NULL;

  {
    char  *buf = (char *)malloc(pos + 1);
    size_t pos2 = 0;
    if (!buf) return NULL;
    r = xjson_strify_impl_(XJSON_C(node), pretty, buf, pos + 1, &pos2, 0);
    if (r != 0) { free(buf); return NULL; }
    buf[pos2] = '\0';
    return buf;
  }
}

char *xJsonStringify(const xJson *node)       { return xjson_stringify_alloc_(node, 0); }
char *xJsonStringifyPretty(const xJson *node) { return xjson_stringify_alloc_(node, 1); }

int xJsonStringifyTo(const xJson *node, int pretty, char *buf, size_t *len) {
  size_t pos = 0;
  size_t cap;
  int r;

  if (!node || !len) return -1;
  cap = *len;

  r = xjson_strify_impl_(XJSON_C(node), pretty, buf, cap, &pos, 0);
  *len = pos + 1;
  if (r != 0 || pos >= cap) return -1;
  if (buf) buf[pos] = '\0';
  return 0;
}
