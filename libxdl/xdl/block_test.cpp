#include <gtest/gtest.h>
extern "C" {
#include <xdl/block.h>
}

TEST(Block, AllocEven) {
  xdl_block_t *b = xdl_block_alloc(4, 256, 1024);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b[0].offset, 0ULL); EXPECT_EQ(b[0].len, 256U);
  EXPECT_EQ(b[1].offset, 256ULL); EXPECT_EQ(b[1].len, 256U);
  EXPECT_EQ(b[2].offset, 512ULL); EXPECT_EQ(b[2].len, 256U);
  EXPECT_EQ(b[3].offset, 768ULL); EXPECT_EQ(b[3].len, 256U);
  xdl_block_free(b);
}

TEST(Block, LastPartial) {
  xdl_block_t *b = xdl_block_alloc(3, 256, 600);
  EXPECT_EQ(b[2].len, 88U);
  xdl_block_free(b);
}

TEST(Block, MarkComplete) {
  xdl_block_t *b = xdl_block_alloc(2, 256, 512);
  xdl_block_mark_complete(&b[0]);
  EXPECT_TRUE(b[0].done);
  EXPECT_FALSE(b[1].done);
  xdl_block_free(b);
}

TEST(Block, Retry) {
  xdl_block_t *b = xdl_block_alloc(1, 256, 256);
  EXPECT_TRUE(xdl_block_retry(&b[0]));
  EXPECT_TRUE(xdl_block_retry(&b[0]));
  EXPECT_TRUE(xdl_block_retry(&b[0]));
  EXPECT_FALSE(xdl_block_retry(&b[0]));
  xdl_block_free(b);
}

TEST(Block, CountComplete) {
  xdl_block_t *b = xdl_block_alloc(3, 256, 768);
  EXPECT_EQ(xdl_block_count_complete(b, 3), 0U);
  EXPECT_EQ(xdl_block_count_pending(b, 3), 3U);
  xdl_block_mark_complete(&b[0]);
  xdl_block_mark_complete(&b[2]);
  EXPECT_EQ(xdl_block_count_complete(b, 3), 2U);
  xdl_block_free(b);
}

TEST(Block, NullSafe) {
  EXPECT_EQ(xdl_block_alloc(0, 256, 0), nullptr);
  xdl_block_free(nullptr);
  xdl_block_mark_complete(nullptr);
  EXPECT_EQ(xdl_block_count_complete(nullptr, 10), 0U);
}
