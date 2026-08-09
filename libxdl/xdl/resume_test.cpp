/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * resume_test.cpp - Resume checkpoint tests
 */

#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>

extern "C" {
#include <xdl/resume.h>
}

static const char *TMP = "/tmp/xdl_resume_test.resume";

TEST(Resume, SaveLoad) {
  uint8_t info[20]; memset(info, 0xAB, 20);
  uint8_t bm[2] = {0x55, 0xAA};

  ASSERT_EQ(xdl_resume_save(TMP, info, bm, 2, 16), 0);

  uint8_t loaded[2] = {0};
  uint32_t bc = 0;
  ASSERT_EQ(xdl_resume_load(TMP, info, loaded, 2, &bc), 0);
  EXPECT_EQ(bc, 16U);
  EXPECT_EQ(loaded[0], 0x55);
  EXPECT_EQ(loaded[1], 0xAA);

  remove(TMP);
}

TEST(Resume, InfoHashMismatch) {
  uint8_t info[20]; memset(info, 0xAB, 20);
  uint8_t bm[1] = {0xFF};
  ASSERT_EQ(xdl_resume_save(TMP, info, bm, 1, 8), 0);

  uint8_t wrong[20]; memset(wrong, 0xCD, 20);
  uint8_t loaded[1];
  uint32_t bc;
  EXPECT_NE(xdl_resume_load(TMP, wrong, loaded, 1, &bc), 0);
  remove(TMP);
}

TEST(Resume, MagicMismatch) {
  FILE *f = fopen(TMP, "wb");
  ASSERT_NE(f, nullptr);
  const char bad[16] = "BAD_MAGIC_\0\0\0\0\0";
  fwrite(bad, 1, 16, f); fclose(f);

  uint8_t info[20] = {0};
  uint8_t loaded[1];
  uint32_t bc;
  EXPECT_NE(xdl_resume_load(TMP, info, loaded, 1, &bc), 0);
  remove(TMP);
}

TEST(Resume, FileNotFound) {
  uint8_t info[20] = {0};
  uint8_t loaded[1];
  uint32_t bc;
  EXPECT_NE(xdl_resume_load("/tmp/nonexistent_xdl_test.resume", info, loaded, 1, &bc), 0);
}

TEST(Resume, SaveNullPath) { EXPECT_NE(xdl_resume_save(nullptr, reinterpret_cast<const uint8_t *>("a"), nullptr, 0, 0), 0); }
TEST(Resume, LoadNullPath) { EXPECT_NE(xdl_resume_load(nullptr, nullptr, nullptr, 0, nullptr), 0); }
TEST(Resume, DeleteNull) { EXPECT_NE(xdl_resume_delete(nullptr), 0); }
TEST(Resume, DeleteNonexistent) { EXPECT_EQ(xdl_resume_delete("/tmp/nonexistent.resume"), 0); }
