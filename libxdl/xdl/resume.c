/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * resume.c - Resume checkpoint implementation
 */

#include <xdl/resume.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int xdl_resume_save(const char *path, const uint8_t info_hash[20],
                const uint8_t *bitmap, uint32_t bitmap_len, uint32_t block_count) {
  if (!path || !info_hash || !bitmap) return -1;
  FILE *f = fopen(path, "wb");
  if (!f) return -1;

  if (fwrite(XDL_RESUME_MAGIC, 1, XDL_RESUME_MAGIC_LEN, f) != XDL_RESUME_MAGIC_LEN) goto fail;
  if (fwrite(info_hash, 1, 20, f) != 20) goto fail;
  if (fwrite(&block_count, sizeof(block_count), 1, f) != 1) goto fail;
  if (fwrite(bitmap, 1, bitmap_len, f) != bitmap_len) goto fail;

  fclose(f);
  return 0;

fail:
  fclose(f);
  return -1;
}

int xdl_resume_load(const char *path, const uint8_t info_hash[20],
                uint8_t *bitmap_out, uint32_t bitmap_cap,
                uint32_t *block_count_out) {
  if (!path || !info_hash || !bitmap_out || !block_count_out) return -1;

  FILE *f = fopen(path, "rb");
  if (!f) return -1;

  char magic[XDL_RESUME_MAGIC_LEN];
  if (fread(magic, 1, XDL_RESUME_MAGIC_LEN, f) != XDL_RESUME_MAGIC_LEN) goto fail;
  if (memcmp(magic, XDL_RESUME_MAGIC, XDL_RESUME_MAGIC_LEN) != 0) goto fail;

  uint8_t stored_hash[20];
  if (fread(stored_hash, 1, 20, f) != 20) goto fail;
  if (memcmp(stored_hash, info_hash, 20) != 0) goto fail;

  uint32_t block_count;
  if (fread(&block_count, sizeof(block_count), 1, f) != 1) goto fail;
  uint32_t bitmap_len = (block_count + 7) / 8;
  if (bitmap_len > bitmap_cap) goto fail;

  if (fread(bitmap_out, 1, bitmap_len, f) != bitmap_len) goto fail;

  *block_count_out = block_count;
  fclose(f);
  return 0;

fail:
  fclose(f);
  return -1;
}

int xdl_resume_delete(const char *path) {
  if (!path) return -1;
  remove(path);
  return 0;
}
