/*
 * cache.h - Chunk cache with Resource→Clip→Block→Piece hierarchy
 */
#ifndef DLP_CACHE_H
#define DLP_CACHE_H

#include <stdint.h>

#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>

typedef struct dlp_cache *dlp_cache_t;

/** Callback for async cache operations. */
typedef void (*dlp_cache_cb)(xErrno err, void *arg);

/** Create a cache instance. @p loop is used for xfs async I/O. */
XCAPI(dlp_cache_t) dlp_cache_init(const char *dir, xEventLoop loop);

/** Destroy the cache. */
XCAPI(void) dlp_cache_deinit(dlp_cache_t c);

/** Open or create a resource by rid. */
XCAPI(xErrno) dlp_cache_open_resource(dlp_cache_t c, const char *rid);

/** Open or create a clip under a resource. */
XCAPI(xErrno) dlp_cache_open_clip(dlp_cache_t c, const char *rid, const char *clip_id,
                                  uint64_t size);

/* ── Async I/O ────────────────────────────────────────────────────── */

/**
 * @brief Write data to a clip asynchronously via xfs.
 *
 * Bitmap updates happen synchronously before the write is dispatched.
 * @p cb fires on the event loop thread when the write completes.
 */
XCAPI(xErrno) dlp_cache_write(dlp_cache_t c, const char *rid, const char *clip_id, uint64_t offset,
                              const uint8_t *data, size_t len, dlp_cache_cb cb, void *arg);

/**
 * @brief Read data from a clip asynchronously via xfs.
 *
 * Checks readiness first. If data is not ready, returns xErrno_NotFound
 * without dispatching I/O.
 * @p cb fires on the event loop thread when the read completes.
 */
XCAPI(xErrno) dlp_cache_read(dlp_cache_t c, const char *rid, const char *clip_id, uint64_t offset,
                             uint8_t *buf, size_t len, dlp_cache_cb cb, void *arg);

/**
 * @brief Update the total file size for a clip (when discovered from CDN).
 * Recalculates the last block's size and re-checks its done status.
 */
XCAPI(xErrno) dlp_cache_set_file_size(dlp_cache_t c, const char *rid, const char *clip_id,
                                      uint64_t file_size);

/**
 * @brief Synchronous readiness check. O(1) for block-aligned queries.
 * @return 1 if all data in [offset, offset+len) is cached, 0 otherwise.
 */
XCAPI(int) dlp_cache_is_ready(dlp_cache_t c, const char *rid, const char *clip_id, uint64_t offset,
                              size_t len);

/**
 * @brief Get the total file size for a clip (0 if unknown).
 */
XCAPI(uint64_t) dlp_cache_get_size(dlp_cache_t c, const char *rid, const char *clip_id);

#endif
