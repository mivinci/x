/*
 * bus.c - Pub/sub notification bus
 */
#include "bus.h"

#include <stdlib.h>
#include <string.h>

#include <x/base/log.h>
#include <x/base/map.h>

struct dlp_sub {
  dlp_bus_cb      cb;
  void           *arg;
  struct dlp_sub *next;
};

struct dlp_bus {
  xMap subs; /* key (str) → dlp_sub* */
};

dlp_bus_t dlp_bus_create(void) {
  struct dlp_bus *b = (struct dlp_bus *)calloc(1, sizeof(*b));
  if (!b) return NULL;
  b->subs = xMapCreate(xMapType_Hash, 64, xMapStrHash, xMapStrEq);
  if (!b->subs) {
    free(b);
    return NULL;
  }
  return b;
}

void dlp_bus_destroy(dlp_bus_t b) {
  if (!b) return;
  /* Free all subscriber lists. Keys are owned by the map's first
   * subscriber's key field, but we use strdup'd keys in subscribe. */
  xMapDestroy(b->subs);
  free(b);
}

xErrno dlp_bus_subscribe(dlp_bus_t b, const char *key, dlp_bus_cb cb, void *arg) {
  if (!b || !key || !cb) return xErrno_InvalidArg;

  struct dlp_sub *s = (struct dlp_sub *)malloc(sizeof(*s));
  if (!s) return xErrno_NoMemory;
  s->cb   = cb;
  s->arg  = arg;
  s->next = NULL;

  /* Prepend to linked list in map; strdup key — xMapSet stores raw pointer */
  char *dup_key = strdup(key);
  if (!dup_key) {
    free(s);
    return xErrno_NoMemory;
  }

  struct dlp_sub *head = (struct dlp_sub *)xMapGet(b->subs, dup_key);
  if (head) {
    s->next = head;
  }
  xMapSet(b->subs, dup_key, s);
  return xErrno_Ok;
}

void dlp_bus_publish(dlp_bus_t b, const char *key) {
  if (!b || !key) return;

  /* Detach the subscriber list to avoid issues if callbacks re-subscribe */
  struct dlp_sub *head = (struct dlp_sub *)xMapGet(b->subs, key);
  if (!head) return;
  xMapSet(b->subs, key, NULL);

  /* Invoke all callbacks synchronously on the event loop thread */
  while (head) {
    struct dlp_sub *next = head->next;
    if (head->cb) head->cb(head->arg);
    free(head);
    head = next;
  }
}
