/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * torrent.h - Torrent metadata struct and parser
 */

#ifndef XDL_TORRENT_H
#define XDL_TORRENT_H

#include <stddef.h>
#include <stdint.h>

#include <x/base/base.h>

/** @brief Maximum number of HTTP fallback URLs. */
#define XDL_TORRENT_MAX_URLS 8
/** @brief Default block size in bytes (256 KiB). */
#define XDL_TORRENT_DEFAULT_BLOCK_LENGTH 262144

/**
 * @brief Parsed .torrent metadata.
 *
 * Caller creates via xdl_torrent_parse() and frees via xdl_torrent_destroy().
 * All fields except http_urls are owned (malloc'd). http_urls entries are
 * individually malloc'd.
 */
XDEF_STRUCT(xdl_torrent_t) {
  char    *name;                         /**< Suggested file name                   */
  uint64_t length;                       /**< Total file size in bytes              */
  uint32_t block_length;                 /**< Block size (default 256 KiB)          */
  uint32_t block_count;                  /**< Number of blocks = ceil(length / block_length) */
  uint8_t *block_hashes;                 /**< block_count * 20 bytes SHA1, may be NULL */
  char    *announce;                     /**< Tracker URL, optional                  */
  char    *http_urls[XDL_TORRENT_MAX_URLS]; /**< HTTP fallback URLs, optional            */
  int      http_url_count;               /**< Number of HTTP URLs                     */
};

/**
 * @brief Parse bencoded .torrent data.
 *
 * Extracts all standard fields: name, length, block_length, blocks (SHA1 hashes),
 * announce, and url-list. Sets block_count from block_hashes or computes it
 * from length / block_length.
 *
 * @param data  Raw bencoded .torrent file content.
 * @param len   Length of @p data in bytes.
 * @return Parsed torrent on success, NULL on parse error.
 *         Caller must free with xdl_torrent_destroy().
 */
XCAPI_LOCAL(xdl_torrent_t *) xdl_torrent_parse(const uint8_t *data, size_t len);

/**
 * @brief Free all memory allocated by xdl_torrent_parse().
 * @param t  Torrent to free (NULL-safe).
 */
XCAPI_LOCAL(void) xdl_torrent_destroy(xdl_torrent_t *t);

/**
 * @brief Return the block count for a torrent.
 * @param t  Torrent (must not be NULL).
 * @return Block count.
 */
XCAPI_LOCAL(uint32_t) xdl_torrent_block_count(const xdl_torrent_t *t);

#endif /* XDL_TORRENT_H */
