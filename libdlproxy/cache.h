/*
 * cache.h - Chunk cache with Resource→Clip→Block→Piece hierarchy
 */
#ifndef DLP_CACHE_H
#define DLP_CACHE_H

#include <stdint.h>
#include <x/base/error.h>

typedef struct dlp_cache *dlp_cache_t;

dlp_cache_t dlp_cache_init(const char *dir);
void       dlp_cache_deinit(dlp_cache_t c);
xErrno     dlp_cache_open_resource(dlp_cache_t c, const char *rid);
xErrno     dlp_cache_open_clip(dlp_cache_t c, const char *rid, const char *clip_id, uint64_t size);
xErrno     dlp_cache_read(dlp_cache_t c, const char *rid, uint64_t offset, uint8_t *buf, size_t len);
xErrno     dlp_cache_write(dlp_cache_t c, const char *rid, uint64_t offset, const uint8_t *data, size_t len);
int        dlp_cache_is_ready(dlp_cache_t c, const char *rid, uint64_t offset, size_t len);

#endif
