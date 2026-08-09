/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * magnet.h - Magnet URI parser
 *
 * Parses BitTorrent-compatible magnet URIs:
 *   magnet:?xt=urn:btih:<info_hash_hex>&dn=<name>&tr=<url>&tr=<url2>
 */

#ifndef XDL_MAGNET_H
#define XDL_MAGNET_H

#include <stddef.h>
#include <stdint.h>
#include <x/base/base.h>

/** @brief Maximum number of tracker URLs in a magnet URI. */
#define XDL_MAGNET_MAX_TRACKERS 8
/** @brief Maximum length of the display name. */
#define XDL_MAGNET_MAX_NAME     256

/**
 * @brief Parsed magnet URI result.
 *
 * Caller initialises via xdl_magnet_parse() and must call xdl_magnet_result_free()
 * to release tracker strings.
 */
XDEF_STRUCT(xdl_magnet_result_t) {
  uint8_t  info_hash[20];                        /**< Raw SHA1 info hash             */
  char     name[XDL_MAGNET_MAX_NAME];               /**< Display name (dn)              */
  char    *trackers[XDL_MAGNET_MAX_TRACKERS];       /**< Tracker URLs (tr, malloc'd)    */
  int      tracker_count;                        /**< Number of tracker entries      */
};

/**
 * @brief Parse a magnet URI.
 * @param uri     Null-terminated magnet URI string.
 * @param result  Output, caller-allocated. Must be freed with xdl_magnet_result_free().
 * @return 0 on success, -1 on error (malformed, missing xt, invalid hex).
 */
XCAPI_LOCAL(int) xdl_magnet_parse(const char *uri, xdl_magnet_result_t *result);

/**
 * @brief Free tracker strings allocated by xdl_magnet_parse().
 * @param result  Result to clean up (NULL-safe).
 */
XCAPI_LOCAL(void) xdl_magnet_result_free(xdl_magnet_result_t *result);

#endif  /* XDL_MAGNET_H */
