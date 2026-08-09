/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * resume.h - Download resume (.resume) checkpoint persistence
 *
 * The .resume file stores the downloader's block completion bitmap,
 * separate from the read-only .torrent file.
 *
 * Format (binary):
 *   Magic:     "XDL_RESUME_V1\0\0"               (16B)
 *   info_hash: uint8_t[20]                        (20B SHA1)
 *   block_count: uint32_t LE                      (4B)
 *   bitmap:     N bytes                           (N = ceil(block_count/8))
 */

#ifndef XDL_RESUME_H
#define XDL_RESUME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <x/base/base.h>

#define XDL_RESUME_MAGIC     "XDL_RESUME_V1\0\0"
#define XDL_RESUME_MAGIC_LEN 16

/**
 * @brief Write a .resume file to disk.
 * @param path         Output file path (e.g. "<dest>.part.resume").
 * @param info_hash    20-byte SHA1 to identify the torrent.
 * @param bitmap       Block completion bitmap bytes.
 * @param bitmap_len   Length of @p bitmap in bytes.
 * @param block_count  Total block count.
 * @return 0 on success, -1 on error.
 */
XCAPI_LOCAL(int)
xdl_resume_save(const char *path, const uint8_t info_hash[20], const uint8_t *bitmap,
            uint32_t bitmap_len, uint32_t block_count);

/**
 * @brief Load a .resume file from disk.
 *
 * Validates magic and info_hash. On mismatch, returns an error and the
 * caller should delete the .resume and start fresh.
 *
 * @param path         Path to .resume file.
 * @param info_hash    Expected 20-byte SHA1.
 * @param bitmap_out   Output buffer for bitmap bytes (caller-allocated).
 * @param bitmap_cap   Capacity of @p bitmap_out in bytes (must be large enough).
 * @param block_count_out  Output: block count from file.
 * @return 0 on success (bitmap loaded and valid), -1 on error (corrupt or mismatched).
 */
XCAPI_LOCAL(int)
xdl_resume_load(const char *path, const uint8_t info_hash[20], uint8_t *bitmap_out, uint32_t bitmap_cap,
            uint32_t *block_count_out);

/**
 * @brief Delete a .resume file.
 * @param path  Path to delete (NULL-safe, non-existent files are ok).
 * @return 0 on success, -1 on error.
 */
XCAPI_LOCAL(int) xdl_resume_delete(const char *path);

#endif /* XDL_RESUME_H */
