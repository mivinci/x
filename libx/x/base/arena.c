/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * arena.c - Fixed-Capacity Bump Allocator (implementation)
 */

#include <stddef.h>
#include <stdlib.h>

#include <x/base/arena.h>

/* ── Internal structure ──────────────────────────────────────────────── */

#define X_ARENA_STRUCT \
  char *begin;         \
  char *pos;           \
  char *end

struct xArenaImpl {
  X_ARENA_STRUCT;
};

/* ── Align-up helper ─────────────────────────────────────────────────── */

static inline char *arena_align_up(char *p, size_t align) {
  size_t i = (size_t)p;
  size_t a = (i + align - 1) & ~(align - 1);
  return (char *)a;
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

xArena *xArenaCreate(size_t capacity) {
  struct xArenaImpl *a;
  char              *buf;

  buf = (char *)malloc(capacity);
  if (!buf) return NULL;

  a = (struct xArenaImpl *)malloc(sizeof(*a));
  if (!a) {
    free(buf);
    return NULL;
  }

  a->begin = buf;
  a->pos   = buf;
  a->end   = buf + capacity;
  return (xArena *)a;
}

void xArenaDestroy(xArena *a) {
  struct xArenaImpl *impl = (struct xArenaImpl *)a;
  if (!impl) return;
  free(impl->begin);
  free(impl);
}

/* ── Allocation ──────────────────────────────────────────────────────── */

void *xArenaAlloc(xArena *a, size_t size) {
  return xArenaAllocAligned(a, size, XARENA_DEFAULT_ALIGN);
}

void *xArenaAllocAligned(xArena *a, size_t size, size_t align) {
  struct xArenaImpl *impl = (struct xArenaImpl *)a;
  char              *p;

  if (align == 0) {
    align = XARENA_DEFAULT_ALIGN;
  }

  p = arena_align_up(impl->pos, align);
  if (p + size > impl->end) return NULL;
  impl->pos = p + size;
  return p;
}

/* ── Queries ─────────────────────────────────────────────────────────── */

size_t xArenaCapacity(const xArena *a) {
  const struct xArenaImpl *impl = (const struct xArenaImpl *)a;
  return (size_t)(impl->end - impl->begin);
}

size_t xArenaUsed(const xArena *a) {
  const struct xArenaImpl *impl = (const struct xArenaImpl *)a;
  return (size_t)(impl->pos - impl->begin);
}

size_t xArenaRemaining(const xArena *a) {
  const struct xArenaImpl *impl = (const struct xArenaImpl *)a;
  return (size_t)(impl->end - impl->pos);
}

int xArenaOwns(const xArena *a, const void *p) {
  const struct xArenaImpl *impl = (const struct xArenaImpl *)a;
  const char              *cp  = (const char *)p;
  return cp >= impl->begin && cp < impl->end;
}

/* ── Bulk Reclaim ────────────────────────────────────────────────────── */

void xArenaReset(xArena *a) {
  struct xArenaImpl *impl = (struct xArenaImpl *)a;
  impl->pos = impl->begin;
}
