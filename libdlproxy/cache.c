/*
 * cache.c - Chunk cache stub (implementation in dlproxy-cache change)
 */
#include "cache.h"
#include <stdlib.h>

dlp_cache_t dlp_cache_init(const char *dir) { (void)dir; return NULL; }
void       dlp_cache_deinit(dlp_cache_t c) { (void)c; }
xErrno     dlp_cache_open_resource(dlp_cache_t c, const char *rid) { (void)c; (void)rid; return xErrno_Ok; }
xErrno     dlp_cache_open_clip(dlp_cache_t c, const char *rid, const char *clip_id, uint64_t size) { (void)c; (void)rid; (void)clip_id; (void)size; return xErrno_Ok; }
xErrno     dlp_cache_read(dlp_cache_t c, const char *rid, uint64_t offset, uint8_t *buf, size_t len) { (void)c; (void)rid; (void)offset; (void)buf; (void)len; return xErrno_NotFound; }
xErrno     dlp_cache_write(dlp_cache_t c, const char *rid, uint64_t offset, const uint8_t *data, size_t len) { (void)c; (void)rid; (void)offset; (void)data; (void)len; return xErrno_Ok; }
int        dlp_cache_is_ready(dlp_cache_t c, const char *rid, uint64_t offset, size_t len) { (void)c; (void)rid; (void)offset; (void)len; return 0; }
