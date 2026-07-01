/*
 * cache_test.cpp - Unit tests for dlp_cache
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include "cache.h"

#include <unistd.h>

#include <gtest/gtest.h>

#include <x/base/error.h>
#include <x/base/event.h>

/* -- Platform helpers --------------------------------------------- */

static const char *test_dir(void) {
  return "/tmp/dlproxy_cache_test";
}
static void rm_test_dir(void) {
  std::string cmd = "rm -rf " + std::string(test_dir());
  system(cmd.c_str());
}
static std::string meta_path_str(const char *rid, const char *clip_id) {
  char buf[512];
  snprintf(buf, sizeof(buf), "%s/%s/%s.meta", test_dir(), rid, clip_id);
  return std::string(buf);
}
static bool file_exists(const char *path) {
  return access(path, F_OK) == 0;
}
static void truncate_file(const char *path, size_t size) {
  truncate(path, static_cast<off_t>(size));
}

class CacheTest : public ::testing::Test {
protected:
  xEventLoop  loop  = nullptr;
  dlp_cache_t cache = nullptr;

  void SetUp() override {
    rm_test_dir();
    loop = xEventLoopCreate();
    xEventLoopEnter(loop);
    cache = dlp_cache_init(test_dir(), loop);
    ASSERT_NE(cache, nullptr);
  }

  void TearDown() override {
    dlp_cache_deinit(cache);
    xEventLoopLeave();
    xEventLoopDestroy(loop);
    rm_test_dir();
  }
};

/* Completion callback for async ops */
struct comp_ctx {
  bool   done;
  xErrno err;
};
static void comp_cb(xErrno err, void *arg) {
  auto *c = reinterpret_cast<comp_ctx *>(arg);
  c->err  = err;
  c->done = true;
  xEventLoopStop(xEventLoopCurrent());
}
static void wait_for(comp_ctx &c, xEventLoop loop) {
  if (!c.done) xEventLoopRun(loop, X_RUN_DEFAULT);
  EXPECT_TRUE(c.done);
}

/* ── Open / Close ────────────────────────────────────────────── */

TEST_F(CacheTest, OpenResourceAndClip) {
  ASSERT_EQ(dlp_cache_open_resource(cache, "test_rid"), xErrno_Ok);
  ASSERT_EQ(dlp_cache_open_resource(cache, "test_rid"), xErrno_Ok);
  ASSERT_EQ(dlp_cache_open_clip(cache, "test_rid", "0", 1024 * 1024), xErrno_Ok);
  ASSERT_EQ(dlp_cache_open_clip(cache, "test_rid", "0", 1024 * 1024), xErrno_Ok);
}

TEST_F(CacheTest, OpenClipRequiresResource) {
  ASSERT_EQ(dlp_cache_open_clip(cache, "no_such", "0", 100), xErrno_NotFound);
}

TEST_F(CacheTest, InvalidArgs) {
  ASSERT_EQ(dlp_cache_open_resource(nullptr, "x"), xErrno_InvalidArg);
  ASSERT_EQ(dlp_cache_open_resource(cache, nullptr), xErrno_InvalidArg);
  ASSERT_EQ(dlp_cache_write(nullptr, "r", "0", 0,
                            reinterpret_cast<uint8_t *>(const_cast<char *>("x")), 1, nullptr,
                            nullptr),
            xErrno_InvalidArg);
  ASSERT_EQ(dlp_cache_write(cache, nullptr, "0", 0,
                            reinterpret_cast<uint8_t *>(const_cast<char *>("x")), 1, nullptr,
                            nullptr),
            xErrno_InvalidArg);
  ASSERT_EQ(dlp_cache_is_ready(nullptr, "r", "0", 0, 1), 0);
  ASSERT_EQ(dlp_cache_is_ready(cache, nullptr, "0", 0, 1), 0);
  ASSERT_EQ(dlp_cache_is_ready(cache, "r", "0", 0, 0), 0);
}

/* ── Write + Read ─────────────────────────────────────────────── */

TEST_F(CacheTest, WriteFullBlock) {
  ASSERT_EQ(dlp_cache_open_resource(cache, "r1"), xErrno_Ok);
  ASSERT_EQ(dlp_cache_open_clip(cache, "r1", "0", 1024 * 1024), xErrno_Ok);

  const size_t BS = 256 * 1024;
  ASSERT_EQ(dlp_cache_is_ready(cache, "r1", "0", 0, BS), 0);

  uint8_t *data = reinterpret_cast<uint8_t *>(malloc(BS));
  memset(data, 0xAB, BS);

  comp_ctx c = {};
  ASSERT_EQ(dlp_cache_write(cache, "r1", "0", 0, data, BS, comp_cb, &c), xErrno_Ok);
  wait_for(c, loop);
  ASSERT_EQ(c.err, xErrno_Ok);

  ASSERT_EQ(dlp_cache_is_ready(cache, "r1", "0", 0, BS), 1);
  ASSERT_EQ(dlp_cache_is_ready(cache, "r1", "0", 0, 2 * BS), 0);
  free(data);
}

TEST_F(CacheTest, ReadBackData) {
  ASSERT_EQ(dlp_cache_open_resource(cache, "r2"), xErrno_Ok);
  ASSERT_EQ(dlp_cache_open_clip(cache, "r2", "0", 1024 * 1024), xErrno_Ok);

  const size_t BS   = 256 * 1024;
  uint8_t     *data = reinterpret_cast<uint8_t *>(malloc(BS));
  for (size_t i = 0; i < BS; i++)
    data[i] = static_cast<uint8_t>(i & 0xFF);

  comp_ctx cw = {};
  ASSERT_EQ(dlp_cache_write(cache, "r2", "0", 0, data, BS, comp_cb, &cw), xErrno_Ok);
  wait_for(cw, loop);

  uint8_t *ver = reinterpret_cast<uint8_t *>(malloc(BS));
  memset(ver, 0, BS);
  comp_ctx cr = {};
  ASSERT_EQ(dlp_cache_read(cache, "r2", "0", 0, ver, BS, comp_cb, &cr), xErrno_Ok);
  wait_for(cr, loop);
  ASSERT_EQ(memcmp(data, ver, BS), 0);
  free(ver);
  free(data);
}

/* ── Multiple resources ────────────────────────────────────────── */

TEST_F(CacheTest, MultipleResources) {
  ASSERT_EQ(dlp_cache_open_resource(cache, "A"), xErrno_Ok);
  ASSERT_EQ(dlp_cache_open_resource(cache, "B"), xErrno_Ok);
  ASSERT_EQ(dlp_cache_open_clip(cache, "A", "0", 1024 * 1024), xErrno_Ok);
  ASSERT_EQ(dlp_cache_open_clip(cache, "B", "0", 1024 * 1024), xErrno_Ok);

  const size_t BS = 256 * 1024;
  uint8_t     *da = reinterpret_cast<uint8_t *>(malloc(BS));
  memset(da, 0xAA, BS);
  uint8_t *db = reinterpret_cast<uint8_t *>(malloc(BS));
  memset(db, 0xBB, BS);

  comp_ctx ca = {};
  ASSERT_EQ(dlp_cache_write(cache, "A", "0", 0, da, BS, comp_cb, &ca), xErrno_Ok);
  wait_for(ca, loop);
  ASSERT_EQ(dlp_cache_is_ready(cache, "A", "0", 0, BS), 1);

  comp_ctx cb = {};
  ASSERT_EQ(dlp_cache_write(cache, "B", "0", 0, db, BS, comp_cb, &cb), xErrno_Ok);
  wait_for(cb, loop);
  ASSERT_EQ(dlp_cache_is_ready(cache, "B", "0", 0, BS), 1);

  /* Cross-contamination: A's data area is not ready under B */
  /* (A and B have different data files, should be independent) */
  ASSERT_EQ(dlp_cache_is_ready(cache, "B", "0", 0, 2 * BS), 0);
  free(da);
  free(db);
}

/* ── Read before write ─────────────────────────────────────────── */

TEST_F(CacheTest, ReadBeforeWrite) {
  ASSERT_EQ(dlp_cache_open_resource(cache, "r3"), xErrno_Ok);
  ASSERT_EQ(dlp_cache_open_clip(cache, "r3", "0", 1024 * 1024), xErrno_Ok);
  uint8_t buf[256];
  ASSERT_EQ(dlp_cache_read(cache, "r3", "0", 0, buf, 256, nullptr, nullptr), xErrno_NotFound);
}

/* ── Auto-grow blocks ──────────────────────────────────────────── */

TEST_F(CacheTest, AutoGrowBlocks) {
  ASSERT_EQ(dlp_cache_open_resource(cache, "r4"), xErrno_Ok);
  ASSERT_EQ(dlp_cache_open_clip(cache, "r4", "0", 0), xErrno_Ok);
  uint64_t off = 500ULL * 1024 * 1024;
  uint8_t  data[1024];
  memset(data, 0xEE, 1024);
  comp_ctx c = {};
  ASSERT_EQ(dlp_cache_write(cache, "r4", "0", off, data, 1024, comp_cb, &c), xErrno_Ok);
  wait_for(c, loop);
  ASSERT_EQ(dlp_cache_is_ready(cache, "r4", "0", off, 1024), 0); /* partial block */
}

/* ── Cross-block write ─────────────────────────────────────────── */

TEST_F(CacheTest, CrossBlockWrite) {
  ASSERT_EQ(dlp_cache_open_resource(cache, "r5"), xErrno_Ok);
  const size_t BS = 256 * 1024;
  ASSERT_EQ(dlp_cache_open_clip(cache, "r5", "0", BS * 3), xErrno_Ok);

  size_t   start = BS - 128, len = 512;
  uint8_t *data = reinterpret_cast<uint8_t *>(malloc(len));
  memset(data, 0xDD, len);
  comp_ctx c = {};
  ASSERT_EQ(dlp_cache_write(cache, "r5", "0", start, data, len, comp_cb, &c), xErrno_Ok);
  wait_for(c, loop);
  free(data);

  ASSERT_EQ(dlp_cache_is_ready(cache, "r5", "0", 0, BS), 0); /* not done */
  ASSERT_EQ(dlp_cache_is_ready(cache, "r5", "0", BS, BS), 0);
}

/* -- .meta persistence ------------------------------------------- */

TEST_F(CacheTest, MetaSaveAndReload) {
  ASSERT_EQ(dlp_cache_open_resource(cache, "meta1"), xErrno_Ok);
  const size_t BS = 256 * 1024;
  ASSERT_EQ(dlp_cache_open_clip(cache, "meta1", "0", BS * 4), xErrno_Ok);

  /* Write full block 0, verify it's ready */
  uint8_t *data = reinterpret_cast<uint8_t *>(malloc(BS));
  memset(data, 0xAA, BS);
  comp_ctx c = {};
  ASSERT_EQ(dlp_cache_write(cache, "meta1", "0", 0, data, BS, comp_cb, &c), xErrno_Ok);
  wait_for(c, loop);
  free(data);
  ASSERT_EQ(dlp_cache_is_ready(cache, "meta1", "0", 0, BS), 1);

  /* Verify .meta file exists */
  ASSERT_TRUE(file_exists(meta_path_str("meta1", "0").c_str()));

  /* Simulate restart: destroy and recreate cache, same dir */
  dlp_cache_deinit(cache);
  cache = dlp_cache_init(test_dir(), loop);
  ASSERT_NE(cache, nullptr);

  /* Re-open resource and clip — should load bitmap from .meta */
  ASSERT_EQ(dlp_cache_open_resource(cache, "meta1"), xErrno_Ok);
  ASSERT_EQ(dlp_cache_open_clip(cache, "meta1", "0", BS * 4), xErrno_Ok);

  /* Block 0 should be ready (loaded from .meta) */
  ASSERT_EQ(dlp_cache_is_ready(cache, "meta1", "0", 0, BS), 1);
  /* Block 1 should NOT be ready */
  ASSERT_EQ(dlp_cache_is_ready(cache, "meta1", "0", BS, BS), 0);
}

TEST_F(CacheTest, MetaDeleteWhenFullyDownloaded) {
  ASSERT_EQ(dlp_cache_open_resource(cache, "meta2"), xErrno_Ok);
  const size_t BS = 256 * 1024;
  /* Create a clip with exactly 1 block */
  ASSERT_EQ(dlp_cache_open_clip(cache, "meta2", "0", BS), xErrno_Ok);

  /* Write full block (entire file) */
  uint8_t *data = reinterpret_cast<uint8_t *>(malloc(BS));
  memset(data, 0xBB, BS);
  comp_ctx c = {};
  ASSERT_EQ(dlp_cache_write(cache, "meta2", "0", 0, data, BS, comp_cb, &c), xErrno_Ok);
  wait_for(c, loop);
  free(data);
  ASSERT_EQ(dlp_cache_is_ready(cache, "meta2", "0", 0, BS), 1);

  /* .meta should be deleted since all blocks are done */
  ASSERT_FALSE(file_exists(meta_path_str("meta2", "0").c_str()));
}

TEST_F(CacheTest, MetaHeaderMismatchDiscarded) {
  ASSERT_EQ(dlp_cache_open_resource(cache, "meta3"), xErrno_Ok);
  const size_t BS = 256 * 1024;
  ASSERT_EQ(dlp_cache_open_clip(cache, "meta3", "0", BS * 2), xErrno_Ok);

  uint8_t *data = reinterpret_cast<uint8_t *>(malloc(BS));
  memset(data, 0xCC, BS);
  comp_ctx c = {};
  ASSERT_EQ(dlp_cache_write(cache, "meta3", "0", 0, data, BS, comp_cb, &c), xErrno_Ok);
  wait_for(c, loop);
  free(data);

  /* Corrupt the .meta file by truncating it */
  truncate_file(meta_path_str("meta3", "0").c_str(), 10);

  /* Re-open — mismatch should discard .meta */
  dlp_cache_deinit(cache);
  cache = dlp_cache_init(test_dir(), loop);
  ASSERT_EQ(dlp_cache_open_resource(cache, "meta3"), xErrno_Ok);
  ASSERT_EQ(dlp_cache_open_clip(cache, "meta3", "0", BS * 2), xErrno_Ok);

  /* Block 0 should NOT be ready (meta was discarded) */
  ASSERT_EQ(dlp_cache_is_ready(cache, "meta3", "0", 0, BS), 0);
}

#endif /* !_WIN32 */
