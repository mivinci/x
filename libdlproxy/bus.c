/*
 * bus.c - Pub/sub notification bus
 */
#include "bus.h"
#include <stdlib.h>
#include <string.h>
#include <x/base/map.h>

struct dlp_bus {
  xMap subs; /* key (str) → subscriber list head */
};

dlp_bus_t dlp_bus_create(void) {
  struct dlp_bus *b = (struct dlp_bus *)calloc(1, sizeof(*b));
  if (!b) return NULL;
  b->subs = xMapCreate(xMapType_Hash, 64, xMapStrHash, xMapStrEq);
  if (!b->subs) { free(b); return NULL; }
  return b;
}

void dlp_bus_destroy(dlp_bus_t b) { /* TODO */ (void)b; }
xErrno dlp_bus_subscribe(dlp_bus_t b, const char *key, dlp_bus_cb cb, void *arg) {
  (void)b; (void)key; (void)cb; (void)arg; return xErrno_Ok;
}
void dlp_bus_publish(dlp_bus_t b, const char *key) { (void)b; (void)key; }
