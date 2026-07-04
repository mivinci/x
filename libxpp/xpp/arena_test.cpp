/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * arena_test.cpp — Tests for Arena<N>.
 */
#include <cstddef>
#include <string>

#include <gtest/gtest.h>
#include <xpp/arena.h>

/* ── Compile-time size guarantees ──────────────────────────────────── */

// Inline: sizeof includes the buffer.
static_assert(sizeof(xpp::Arena<128>) >= 128, "Inline Arena must include buffer");
static_assert(sizeof(xpp::Arena<256>) >= 256, "Inline Arena must include buffer");

// Heap: sizeof should NOT include the buffer.
static_assert(sizeof(xpp::Arena<4096>) < 256, "Heap Arena must not include full buffer");
static_assert(sizeof(xpp::Arena<65536>) < 256, "Heap Arena must not include full buffer");

/* ── Basic allocation ──────────────────────────────────────────────── */

TEST(ArenaTest, AllocateWithinCapacity) {
  xpp::Arena<256> a;
  void           *p = a.allocate(64);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(a.used(), 64u);
  EXPECT_EQ(a.remaining(), 256u - 64u);
}

TEST(ArenaTest, AllocateFullCapacity) {
  xpp::Arena<256> a;
  void           *p = a.allocate(256);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(a.remaining(), 0u);
}

TEST(ArenaTest, AllocateReturnsNullOnOverflow) {
  xpp::Arena<64> a;
  void          *p1 = a.allocate(48);
  ASSERT_NE(p1, nullptr);
  void *p2 = a.allocate(32);
  EXPECT_EQ(p2, nullptr);
}

/* ── Alignment ─────────────────────────────────────────────────────── */

TEST(ArenaTest, DefaultAlignmentIsMaxAlignT) {
  xpp::Arena<256> a;
  void           *p = a.allocate(1);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t), 0u);
}

TEST(ArenaTest, CustomAlignment) {
  xpp::Arena<256> a;
  void           *p = a.allocate(32, 64);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 64, 0u);
}

TEST(ArenaTest, AlignmentPaddingAccounted) {
  xpp::Arena<256> a;
  a.allocate(1); // 1 byte, pos now at begin+16 (aligned to max_align_t)
  void *p2 = a.allocate(16);
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p2) % alignof(std::max_align_t), 0u);
}

/* ── Contiguity ────────────────────────────────────────────────────── */

TEST(ArenaTest, AllocationsAreContiguous) {
  xpp::Arena<256> a;
  // Use align=1 to avoid padding, so we can check exact contiguity.
  void *p1 = a.allocate(16, 1);
  void *p2 = a.allocate(16, 1);
  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(static_cast<char *>(p2), static_cast<char *>(p1) + 16);
}

/* ── owns() ────────────────────────────────────────────────────────── */

TEST(ArenaTest, OwnsArenaPointer) {
  xpp::Arena<256> a;
  void           *p = a.allocate(32);
  EXPECT_TRUE(a.owns(p));
}

TEST(ArenaTest, DoesNotOwnHeapPointer) {
  xpp::Arena<256> a;
  void           *heap_ptr = ::operator new(32);
  EXPECT_FALSE(a.owns(heap_ptr));
  ::operator delete(heap_ptr);
}

TEST(ArenaTest, DoesNotOwnNullptr) {
  xpp::Arena<256> a;
  EXPECT_FALSE(a.owns(nullptr));
}

TEST(ArenaTest, DoesNotOwnStackPointer) {
  xpp::Arena<256> a;
  int             x = 42;
  EXPECT_FALSE(a.owns(&x));
}

/* ── reset() ───────────────────────────────────────────────────────── */

TEST(ArenaTest, ResetClearsArena) {
  xpp::Arena<256> a;
  a.allocate(128);
  EXPECT_EQ(a.used(), 128u);
  a.reset();
  EXPECT_EQ(a.used(), 0u);
  EXPECT_EQ(a.remaining(), 256u);
}

TEST(ArenaTest, ResetThenReallocate) {
  xpp::Arena<256> a;
  void           *p1 = a.allocate(128);
  ASSERT_NE(p1, nullptr);
  a.reset();
  void *p2 = a.allocate(128);
  ASSERT_NE(p2, nullptr);
  // After reset, the new allocation should start from the beginning.
  EXPECT_EQ(p2, p1);
}

TEST(ArenaTest, MultipleResets) {
  xpp::Arena<256> a;
  for (int i = 0; i < 10; ++i) {
    a.allocate(64);
    a.reset();
    EXPECT_EQ(a.used(), 0u);
  }
}

/* ── make<T>() ─────────────────────────────────────────────────────── */

TEST(ArenaTest, MakeConstructsObject) {
  xpp::Arena<256> a;
  auto           *p = a.make<std::string>("hello");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(*p, "hello");
  EXPECT_TRUE(a.owns(p));
}

TEST(ArenaTest, MakeReturnsNullOnOverflow) {
  xpp::Arena<32> a;
  // sizeof(std::string) is typically 24-32 bytes; might fit or not.
  // Use a larger type to guarantee overflow.
  struct Big {
    char data[64];
  };
  auto *p = a.make<Big>();
  EXPECT_EQ(p, nullptr);
}

TEST(ArenaTest, MakeMultipleObjects) {
  xpp::Arena<256> a;
  auto           *p1 = a.make<int>(42);
  auto           *p2 = a.make<int>(99);
  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(*p1, 42);
  EXPECT_EQ(*p2, 99);
  EXPECT_TRUE(a.owns(p1));
  EXPECT_TRUE(a.owns(p2));
}

/* ── Capacity queries ──────────────────────────────────────────────── */

TEST(ArenaTest, FreshArenaQueries) {
  xpp::Arena<128> a;
  EXPECT_EQ(a.total_capacity(), 128u);
  EXPECT_EQ(a.used(), 0u);
  EXPECT_EQ(a.remaining(), 128u);
}

TEST(ArenaTest, QueriesAfterAllocation) {
  xpp::Arena<128> a;
  a.allocate(32);
  EXPECT_GE(a.used(), 32u);
  EXPECT_LE(a.remaining(), 96u);
  EXPECT_EQ(a.total_capacity(), 128u);
}

/* ── Inline vs heap storage ────────────────────────────────────────── */

TEST(ArenaTest, SmallArenaIsInline) {
  // sizeof includes the buffer → inline storage.
  EXPECT_LT(sizeof(xpp::Arena<64>), 100u);
  EXPECT_LT(sizeof(xpp::Arena<128>), 200u);
  EXPECT_LT(sizeof(xpp::Arena<256>), 300u);
}

TEST(ArenaTest, LargeArenaIsHeap) {
  // sizeof does NOT include the buffer → heap storage.
  // Should be just pointers (3 pointers ≈ 24 bytes).
  EXPECT_LT(sizeof(xpp::Arena<4096>), 100u);
  EXPECT_LT(sizeof(xpp::Arena<65536>), 100u);
}

TEST(ArenaTest, LargeArenaWorks) {
  xpp::Arena<4096> a;
  void            *p = a.allocate(2048);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(a.owns(p));
  EXPECT_EQ(a.total_capacity(), 4096u);
  EXPECT_EQ(a.used(), 2048u);
}

/* ── Move semantics ────────────────────────────────────────────────── */

TEST(ArenaTest, MoveLargeArena) {
  xpp::Arena<4096> a;
  void            *p1 = a.allocate(64);
  ASSERT_NE(p1, nullptr);

  xpp::Arena<4096> b(std::move(a));
  EXPECT_TRUE(b.owns(p1));
  EXPECT_EQ(b.used(), 64u);
  // Old arena is invalidated.
  EXPECT_EQ(a.remaining(), 0u);
}

TEST(ArenaTest, MoveSmallArena) {
  xpp::Arena<128> a;
  void           *p1 = a.allocate(32, 1);
  ASSERT_NE(p1, nullptr);

  xpp::Arena<128> b(std::move(a));
  // b should have the same used amount.
  EXPECT_EQ(b.used(), 32u);
  EXPECT_EQ(b.remaining(), 96u);
  // b's buffer has the data (memcpy'd from a).
  // p1 pointed into a's buffer; b's buffer is a copy.
  // We can't compare pointers, but we can check b's state.
}
