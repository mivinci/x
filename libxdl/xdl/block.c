/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * block.c - Block management implementation
 */

#include <xdl/block.h>
#include <stdlib.h>
#include <string.h>

xdl_block_t *xdl_block_alloc(uint32_t count, uint32_t block_length, uint64_t file_length) {
  if (count == 0) return NULL;
  xdl_block_t *blocks = (xdl_block_t *)calloc(count, sizeof(xdl_block_t));
  if (!blocks) return NULL;
  for (uint32_t i = 0; i < count; i++) {
    blocks[i].offset = (uint64_t)i * block_length;
    if (i == count - 1 && file_length > 0) {
      uint64_t remaining = file_length - blocks[i].offset;
      blocks[i].len = remaining < block_length ? (uint32_t)remaining : block_length;
    } else {
      blocks[i].len = block_length;
    }
    blocks[i].done = false;
    blocks[i].retries = 0;
  }
  return blocks;
}

void xdl_block_free(xdl_block_t *blocks) { free(blocks); }

void xdl_block_mark_complete(xdl_block_t *b) {
  if (!b) return;
  b->done = true;
  b->retries = 0;
}

bool xdl_block_retry(xdl_block_t *b) {
  if (!b) return false;
  if (b->retries >= 3) return false;
  b->retries++;
  return true;
}

uint32_t xdl_block_count_complete(const xdl_block_t *blocks, uint32_t count) {
  if (!blocks) return 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < count; i++) if (blocks[i].done) n++;
  return n;
}

uint32_t xdl_block_count_pending(const xdl_block_t *blocks, uint32_t count) {
  if (!blocks) return 0;
  return count - xdl_block_count_complete(blocks, count);
}
