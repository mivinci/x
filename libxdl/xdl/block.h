/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * block.h - Block management and bitmap operations
 */

#ifndef XDL_BLOCK_H
#define XDL_BLOCK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <x/base/base.h>

/**
 * @brief A download block.
 *
 * Block is the scheduler's atomic unit: it is assigned to a source (HTTP or P2P)
 * as a whole for scheduling, then decomposed into pieces for P2P transfer.
 */
XDEF_STRUCT(xdl_block_t) {
  uint64_t offset;  /**< Byte offset within the file                      */
  uint32_t len;     /**< Length in bytes (block_length, last may be smaller) */
  bool     done;    /**< True if fully downloaded and SHA1-verified        */
  int      retries; /**< Number of retry attempts so far (max 3)           */
};

/**
 * @brief Allocate and initialise an array of blocks for a file.
 *
 * @param count         Number of blocks to allocate.
 * @param block_length  Block size in bytes.
 * @param file_length   Total file size in bytes.
 * @return Array of @p count xdl_block_t structs (calloc'd), or NULL on error.
 */
XCAPI_LOCAL(xdl_block_t *) xdl_block_alloc(uint32_t count, uint32_t block_length,
                                   uint64_t file_length);

/**
 * @brief Free a block array allocated by xdl_block_alloc().
 * @param blocks  Block array to free (NULL-safe).
 */
XCAPI_LOCAL(void) xdl_block_free(xdl_block_t *blocks);

/**
 * @brief Mark a block as complete (done = true, retries = 0).
 * @param b  Block to mark.
 */
XCAPI_LOCAL(void) xdl_block_mark_complete(xdl_block_t *b);

/**
 * @brief Mark a block for retry (increment retry counter).
 * @param b  Block to retry.
 * @return true if retry is allowed (retries < 3), false if max retries exceeded.
 */
XCAPI_LOCAL(bool) xdl_block_retry(xdl_block_t *b);

/**
 * @brief Count the number of completed blocks.
 * @param blocks  Block array.
 * @param count   Number of blocks.
 * @return Number of blocks with done == true.
 */
XCAPI_LOCAL(uint32_t) xdl_block_count_complete(const xdl_block_t *blocks, uint32_t count);

/**
 * @brief Count the number of blocks not yet done.
 * @param blocks  Block array.
 * @param count   Number of blocks.
 * @return Number of blocks with done == false.
 */
XCAPI_LOCAL(uint32_t) xdl_block_count_pending(const xdl_block_t *blocks, uint32_t count);

#endif  /* XDL_BLOCK_H */
