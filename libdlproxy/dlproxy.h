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

/** Operation mode. */
XDEF_ENUM(dlp_mode_t) {
  DL_MODE_POLL,    /**< Caller drives the event loop via dlp_poll()       */
  DL_MODE_DETACHED, /**< Background thread runs the event loop            */
};

/** Per-task configuration. */
XDEF_STRUCT(dlp_task_conf_t) {
  const char *rid;   /**< Resource identifier (used in player URL path)   */
  const char *url;   /**< Upstream CDN URL for the resource               */
  uint64_t    size;  /**< Total file size in bytes, or 0 if unknown       */
};

/** Global configuration. */
XDEF_STRUCT(dlp_conf_t) {
  uint16_t    port;        /**< Proxy listen port (default 19080)         */
  const char *cache_dir;   /**< Cache directory path (default "./cache")   */
};

/* ── Lifecycle ──────────────────────────────────────────────────────── */

/**
 * @brief Create a dlproxy context.
 * @param conf Configuration, or NULL for defaults.
 * @return New context handle, or NULL on failure.
 */
XCAPI(dlp_ctx_t) dlp_init(const dlp_conf_t *conf);

/**
 * @brief Run the proxy in the specified mode.
 *
 * POLL:   the calling thread enters the event loop and blocks until
 *         dlp_stop() is called from a callback or another thread.
 *
 * DETACHED: a background thread is spawned to run the event loop.
 *           dlp_run() returns immediately.
 *
 * @param ctx  Context from dlp_init().
 * @param mode Operation mode.
 * @return xErrno_Ok on success.
 */
XCAPI(xErrno) dlp_run(dlp_ctx_t ctx, dlp_mode_t mode);

/**
 * @brief Stop a running proxy and release all resources.
 *
 * Posts a deinit request to the event loop. In POLL mode the caller
 * must ensure it is not currently blocked in dlp_poll(). In DETACHED
 * mode the background thread will exit.
 *
 * @param ctx Context from dlp_init().
 */
XCAPI(void) dlp_stop(dlp_ctx_t ctx);

/**
 * @brief Return the port the proxy is listening on.
 * @return Port number, or 0 if not running.
 */
XCAPI(uint16_t) dlp_port(dlp_ctx_t ctx);

/* ── Task management (DETACHED mode only) ──────────────────────────── */

/**
 * @brief Add a video resource for the proxy to cache.
 *
 * The proxy will serve /:rid URLs using the given CDN URL as upstream.
 * In DETACHED mode this is posted to the event loop and returns
 * immediately. In POLL mode it runs synchronously.
 *
 * @param ctx   Context.
 * @param conf  Task configuration.
 * @return xErrno_Ok on success.
 */
XCAPI(xErrno) dlp_task_add(dlp_ctx_t ctx, const dlp_task_conf_t *conf);

#endif /* DLPROXY_H */
