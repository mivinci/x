/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns_cache.c - TTL-based DNS cache backed by xMap
 */

#include "dns_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x/base/map.h>
#include <x/base/time.h>

/* ───────────────────── Types ───────────────────── */

typedef struct {
  char       *key;       /* owned — same pointer stored as the map key */
  xDnsRecord *records;   /* owned list (cloned)                        */
  uint64_t    expiry_ms; /* absolute monotonic time in ms              */
} dns_cache_entry_t;

/* ───────────────────── Key helpers ───────────────────── */

/* Build "name:qtype" key. Caller frees the result. */
static char *make_key(const char *name, uint16_t qtype) {
  size_t nl = strlen(name);
  /* qtype as decimal: up to 5 digits + ':' + NUL */
  size_t cap = nl + 16;
  char *k = (char *)malloc(cap);
  if (!k) return NULL;
  snprintf(k, cap, "%s:%u", name, (unsigned)qtype);
  return k;
}

/* ───────────────────── Entry lifecycle ───────────────────── */

static void entry_free(void *val) {
  dns_cache_entry_t *e = (dns_cache_entry_t *)val;
  if (!e) return;
  free(e->key);
  dns_records_free(e->records);
  free(e);
}

/* xMap iteration callback: free each entry (which owns its key). */
static bool entry_iter_free(const void *key, void *val, void *arg) {
  (void)key;
  (void)arg;
  entry_free(val);
  return true;
}

/* ───────────────────── Public API ───────────────────── */

xMap dns_cache_create(void) {
  return xMapCreate(xMapType_Hash, 64, xMapStrHash, xMapStrEq);
}

void dns_cache_destroy(xMap cache) {
  if (!cache) return;
  xMapIterate(cache, entry_iter_free, NULL);
  xMapDestroy(cache);
}

xDnsRecord *dns_cache_lookup(xMap cache, const char *name, uint16_t qtype) {
  if (!cache || !name) return NULL;
  char *key = make_key(name, qtype);
  if (!key) return NULL;

  dns_cache_entry_t *e = (dns_cache_entry_t *)xMapGet(cache, key);
  if (!e) {
    free(key);
    return NULL;
  }

  if (xMonoMs() >= e->expiry_ms) {
    /* expired — evict. xMapDel returns the value; free it and the key. */
    void *val = xMapDel(cache, key);
    free(key);
    entry_free(val);
    return NULL;
  }
  free(key);
  return dns_records_clone(e->records);
}

void dns_cache_insert(xMap cache, const char *name, uint16_t qtype,
                      const xDnsRecord *records, uint32_t ttl) {
  if (!cache || !name) return;

  char *key = make_key(name, qtype);
  if (!key) return;

  /* Replace any existing entry. */
  void *old = xMapDel(cache, key);
  if (old) entry_free(old);

  dns_cache_entry_t *e =
    (dns_cache_entry_t *)calloc(1, sizeof(dns_cache_entry_t));
  if (!e) {
    free(key);
    return;
  }
  e->key       = key; /* entry owns the key; map stores the same pointer */
  e->records   = dns_records_clone(records);
  e->expiry_ms = xMonoMs() + (uint64_t)ttl * 1000;
  if (e->expiry_ms == 0) e->expiry_ms = 1; /* avoid 0 = "never set" */

  if (xMapSet(cache, key, e) != xErrno_Ok) {
    entry_free(e); /* frees key too */
  }
  /* On success, both the map and e->key reference the same pointer; that's
   * fine — we only free it once (via entry_free, either at eviction or
   * destroy time). */
}
