/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * bencode.h - BitTorrent bencoding parser/writer
 *
 * Bencoding supports four types:
 *  - String: <decimal-length>:<bytes>          e.g. "4:spam"
 *  - Integer: i<decimal-number>e               e.g. "i3e", "i-3e"
 *  - List: l<items>e                           e.g. "l4:spam4:eggse"
 *  - Dictionary: d<key><value>...e             keys must be sorted
 */

#ifndef XDL_BENCODE_H
#define XDL_BENCODE_H

#include <stddef.h>
#include <stdint.h>
#include <x/base/base.h>

/** @brief Bencode value type. */
XDEF_ENUM(xdl_bencode_type_t) {
  XDL_BENCODE_STRING = 0, /**< String: <decimal-length>:<bytes> */
  XDL_BENCODE_INTEGER,    /**< Integer: i<decimal-number>e      */
  XDL_BENCODE_LIST,       /**< List: l<items>e                  */
  XDL_BENCODE_DICT,       /**< Dictionary: d<key><value>...e    */
};

/**
 * @brief Bencode value node in a parsed tree.
 *
 * Strings own their data (malloc'd copy). Integers are stored as int64_t.
 * Lists and dicts own their children recursively.
 */
XDEF_STRUCT(xdl_bencode_value_t) {
  xdl_bencode_type_t type;
  union {
    struct { const uint8_t *data; size_t len; } str; /**< String payload    */
    int64_t integer;                                  /**< Integer value     */
    struct { xdl_bencode_value_t **items; size_t count; } list; /**< List items   */
    struct { xdl_bencode_value_t **keys; xdl_bencode_value_t **values; size_t count; } dict; /**< Dict entries */
  };
};

/**
 * @brief Parse a bencoded byte buffer into a value tree.
 * @param data  Pointer to bencoded data (binary-safe, may contain NUL bytes).
 * @param len   Length of @p data in bytes.
 * @param out   Output: pointer to the parsed value. Caller must free with
 *              xdl_bencode_value_free().
 * @return 0 on success, -1 on parse error.
 */
XCAPI_LOCAL(int) xdl_bencode_parse(const uint8_t *data, size_t len, xdl_bencode_value_t **out);

/**
 * @brief Serialize a value tree back into bencoded bytes.
 *
 * Dictionary keys are written in the order they appear in the value tree
 * (caller must sort via xdl_bencode_dict_sort() for deterministic output).
 *
 * @param value    Value tree to serialize.
 * @param out      Output buffer (malloc'd, caller must free).
 * @param out_len  Output: length of @p out in bytes.
 * @return 0 on success, -1 on error.
 */
XCAPI_LOCAL(int) xdl_bencode_write(const xdl_bencode_value_t *value, uint8_t **out, size_t *out_len);

/**
 * @brief Sort dictionary keys lexicographically by raw byte comparison.
 *
 * Required before computing info_hash (SHA1 of bencoded info dict)
 * as BitTorrent mandates sorted dictionary keys.
 *
 * @param dict  A BENCODE_DICT value (no-op if not a dict).
 */
XCAPI_LOCAL(void) xdl_bencode_dict_sort(xdl_bencode_value_t *dict);

/**
 * @brief Free a parsed value tree recursively.
 * @param value  Value to free (NULL-safe).
 */
XCAPI_LOCAL(void) xdl_bencode_value_free(xdl_bencode_value_t *value);

/**
 * @brief Look up a key in a dictionary.
 * @param dict  Dictionary value.
 * @param key   Null-terminated key string.
 * @return Pointer to the value for @p key, or NULL if not found.
 */
XCAPI_LOCAL(xdl_bencode_value_t *) xdl_bencode_dict_find(xdl_bencode_value_t *dict, const char *key);

/**
 * @brief Get the raw byte range of the "info" dict within a .torrent file.
 *
 * Scans a top-level bencoded dict for key "info" and returns a pointer
 * to the raw bytes of its VALUE. Used to compute info_hash = SHA1(raw).
 *
 * @param data   Raw .torrent file content.
 * @param len    Length of @p data.
 * @param start  Output: pointer into @p data where the info value begins.
 * @param slen   Output: length of the info value bytes.
 * @return 0 on success, -1 if "info" key not found or parse error.
 */
XCAPI_LOCAL(int) xdl_bencode_info_range(const uint8_t *data, size_t len,
                                    const uint8_t **start, size_t *slen);

#endif  /* XDL_BENCODE_H */
