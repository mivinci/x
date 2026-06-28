/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dlproxy.h - HTTP download proxy for video streaming
 */
#ifndef DLPROXY_H
#define DLPROXY_H

#include <stdint.h>
#include <x/base/base.h>
#include <x/base/error.h>

XDEF_HANDLE(dlp_ctx_t);
XDEF_HANDLE(dlp_task_t);

/** Operation mode. */
XDEF_ENUM(dlp_mode_t) {
  DL_MODE_POLL,      /**< Caller drives the event loop via dlp_run()       */
  DL_MODE_DETACHED,   /**< Background thread runs the event loop            */
};

/** Per-task configuration. */
XDEF_STRUCT(dlp_task_conf_t) {
  const char *rid;    /**< Resource identifier (used in player URL path)    */
  const char *url;    /**< Upstream CDN URL for the resource                */
  uint64_t    size;   /**< Total file size in bytes, or 0 if unknown        */
  uint32_t    bitrate; /**< Estimated bitrate in bytes/sec (0 = auto)       */
};

/** Global configuration. */
XDEF_STRUCT(dlp_conf_t) {
  uint16_t    port;        /**< Proxy listen port (default 19080)          */
  const char *cache_dir;   /**< Cache directory path (default "./cache")    */
  int         emergency_ms;/**< Emergency buffer threshold (default 10000)  */
  int         safe_ms;     /**< Safe buffer threshold (default 30000)       */
};

/* ── Lifecycle ─────────────────────────────────────────────────────── */

XCAPI(dlp_ctx_t) dlp_init(const dlp_conf_t *conf);
XCAPI(xErrno)    dlp_run(dlp_ctx_t ctx, dlp_mode_t mode);
XCAPI(void)      dlp_stop(dlp_ctx_t ctx);
XCAPI(uint16_t)  dlp_port(dlp_ctx_t ctx);

/* ── Task management ───────────────────────────────────────────────── */

/**
 * @brief Create a task: register rid → url mapping and init cache.
 * @return Opaque task handle, or NULL on failure.
 */
XCAPI(dlp_task_t) dlp_task_create(dlp_ctx_t ctx, const dlp_task_conf_t *conf);

/**
 * @brief Start playback-position-driven scheduling.
 *
 * A 1-second internal timer fires the scheduler tick. The scheduler
 * downloads blocks near the playback position when the buffer runs low
 * (remain_time < emergency_ms) and continues while below safe_ms.
 */
XCAPI(xErrno) dlp_task_start(dlp_task_t task);

/**
 * @brief Stop scheduling and cancel any in-flight download.
 */
XCAPI(xErrno) dlp_task_stop(dlp_task_t task);

/**
 * @brief Destroy a task: stop scheduling, remove from proxy, free resources.
 */
XCAPI(void) dlp_task_destroy(dlp_task_t task);

#endif /* DLPROXY_H */
