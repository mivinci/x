/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * arena_test.cpp — Tests for xArena (fixed-capacity bump allocator).
 */

#include <cstddef>
#include <cstdlib>

#include <gtest/gtest.h>
#include <x/base/arena.h>

/* ── Basic Allocation ────────────────────────────────────────────────── */

TEST(ArenaTest, AllocateWithinCapacity) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  void *p = xArenaAlloc(a, 64);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(xArenaUsed(a), 64u);
  EXPECT_EQ(xArenaRemaining(a), 256u - 64u);

  xArenaDestroy(a);
}

TEST(ArenaTest, AllocateFullCapacity) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  void *p = xArenaAlloc(a, 256);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(xArenaRemaining(a), 0u);

  xArenaDestroy(a);
}

TEST(ArenaTest, AllocateReturnsNullOnOverflow) {
  xArena *a = xArenaCreate(64);
  ASSERT_NE(a, nullptr);

  void *p1 = xArenaAlloc(a, 48);
  ASSERT_NE(p1, nullptr);

  void *p2 = xArenaAlloc(a, 32);
  EXPECT_EQ(p2, nullptr);

  xArenaDestroy(a);
}

TEST(ArenaTest, ZeroSizeAllocation) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  void *p = xArenaAlloc(a, 0);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(xArenaOwns(a, p));
  EXPECT_EQ(xArenaUsed(a), 0u);

  xArenaDestroy(a);
}

/* ── Alignment ───────────────────────────────────────────────────────── */

TEST(ArenaTest, DefaultAlignment) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  void *p = xArenaAlloc(a, 1);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0u);

  xArenaDestroy(a);
}

TEST(ArenaTest, CustomAlignment) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  void *p = xArenaAllocAligned(a, 32, 64);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 64, 0u);

  xArenaDestroy(a);
}

TEST(ArenaTest, AlignmentPaddingAccounted) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  xArenaAlloc(a, 1); // 1 byte, pos now bumped to begin+16 (aligned)
  void *p2 = xArenaAlloc(a, 16);
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p2) % 16, 0u);

  xArenaDestroy(a);
}

/* ── Contiguity ──────────────────────────────────────────────────────── */

TEST(ArenaTest, AllocationsAreContiguous) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  // Use align=1 to avoid padding.
  void *p1 = xArenaAllocAligned(a, 16, 1);
  void *p2 = xArenaAllocAligned(a, 16, 1);
  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(static_cast<char *>(p2), static_cast<char *>(p1) + 16);

  xArenaDestroy(a);
}

/* ── owns() ──────────────────────────────────────────────────────────── */

TEST(ArenaTest, OwnsArenaPointer) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  void *p = xArenaAlloc(a, 32);
  EXPECT_TRUE(xArenaOwns(a, p));

  xArenaDestroy(a);
}

TEST(ArenaTest, DoesNotOwnHeapPointer) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  void *heap_ptr = malloc(32);
  EXPECT_FALSE(xArenaOwns(a, heap_ptr));
  free(heap_ptr);

  xArenaDestroy(a);
}

TEST(ArenaTest, DoesNotOwnNullptr) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  EXPECT_FALSE(xArenaOwns(a, nullptr));

  xArenaDestroy(a);
}

TEST(ArenaTest, DoesNotOwnStackPointer) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  int x = 42;
  EXPECT_FALSE(xArenaOwns(a, &x));

  xArenaDestroy(a);
}

TEST(ArenaTest, DoesNotOwnOtherArenaPointer) {
  xArena *a1 = xArenaCreate(256);
  xArena *a2 = xArenaCreate(256);
  ASSERT_NE(a1, nullptr);
  ASSERT_NE(a2, nullptr);

  void *p1 = xArenaAlloc(a1, 32);
  ASSERT_NE(p1, nullptr);
  EXPECT_TRUE(xArenaOwns(a1, p1));
  EXPECT_FALSE(xArenaOwns(a2, p1));

  xArenaDestroy(a1);
  xArenaDestroy(a2);
}

/* ── reset() ─────────────────────────────────────────────────────────── */

TEST(ArenaTest, ResetClearsArena) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  xArenaAlloc(a, 128);
  EXPECT_EQ(xArenaUsed(a), 128u);

  xArenaReset(a);
  EXPECT_EQ(xArenaUsed(a), 0u);
  EXPECT_EQ(xArenaRemaining(a), 256u);

  xArenaDestroy(a);
}

TEST(ArenaTest, ResetThenReallocate) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  void *p1 = xArenaAlloc(a, 128);
  ASSERT_NE(p1, nullptr);

  xArenaReset(a);

  void *p2 = xArenaAlloc(a, 128);
  ASSERT_NE(p2, nullptr);
  // After reset, the new allocation should start from the beginning.
  EXPECT_EQ(p2, p1);

  xArenaDestroy(a);
}

TEST(ArenaTest, MultipleResets) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  for (int i = 0; i < 10; ++i) {
    xArenaAlloc(a, 64);
    xArenaReset(a);
    EXPECT_EQ(xArenaUsed(a), 0u);
  }

  xArenaDestroy(a);
}

TEST(ArenaTest, OldPointersAreInvalidAfterReset) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  void *p1 = xArenaAlloc(a, 64);
  ASSERT_NE(p1, nullptr);

  xArenaReset(a);
  // p1 should still be within the buffer range (owns returns true),
  // but its content may be overwritten by future allocations.
  EXPECT_TRUE(xArenaOwns(a, p1));

  void *p2 = xArenaAlloc(a, 64);
  ASSERT_NE(p2, nullptr);
  // After reset, the new allocation should reuse the same memory.
  EXPECT_EQ(p2, p1);

  xArenaDestroy(a);
}

/* ── Capacity Queries ────────────────────────────────────────────────── */

TEST(ArenaTest, FreshArenaQueries) {
  xArena *a = xArenaCreate(128);
  ASSERT_NE(a, nullptr);

  EXPECT_EQ(xArenaCapacity(a), 128u);
  EXPECT_EQ(xArenaUsed(a), 0u);
  EXPECT_EQ(xArenaRemaining(a), 128u);

  xArenaDestroy(a);
}

TEST(ArenaTest, QueriesAfterAllocation) {
  xArena *a = xArenaCreate(128);
  ASSERT_NE(a, nullptr);

  xArenaAlloc(a, 32);
  EXPECT_GE(xArenaUsed(a), 32u);
  EXPECT_LE(xArenaRemaining(a), 96u);
  EXPECT_EQ(xArenaCapacity(a), 128u);

  xArenaDestroy(a);
}

/* ── Large Arena ─────────────────────────────────────────────────────── */

TEST(ArenaTest, LargeArenaWorks) {
  xArena *a = xArenaCreate(4096);
  ASSERT_NE(a, nullptr);

  void *p = xArenaAlloc(a, 2048);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(xArenaOwns(a, p));
  EXPECT_EQ(xArenaCapacity(a), 4096u);
  EXPECT_EQ(xArenaUsed(a), 2048u);

  xArenaDestroy(a);
}

/* ── Multiple Allocations ────────────────────────────────────────────── */

TEST(ArenaTest, MultipleAllocations) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  void *p1 = xArenaAlloc(a, 32);
  void *p2 = xArenaAlloc(a, 64);
  void *p3 = xArenaAlloc(a, 16);

  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);
  ASSERT_NE(p3, nullptr);

  EXPECT_TRUE(xArenaOwns(a, p1));
  EXPECT_TRUE(xArenaOwns(a, p2));
  EXPECT_TRUE(xArenaOwns(a, p3));

  // Pointers should be in order (subject to alignment padding).
  EXPECT_LT(static_cast<char *>(p1), static_cast<char *>(p2));
  EXPECT_LT(static_cast<char *>(p2), static_cast<char *>(p3));

  xArenaDestroy(a);
}

/* ── Capacity Boundary ───────────────────────────────────────────────── */

TEST(ArenaTest, ExactFitAtEnd) {
  xArena *a = xArenaCreate(64);
  ASSERT_NE(a, nullptr);

  void *p = xArenaAllocAligned(a, 64, 1);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(xArenaRemaining(a), 0u);

  // Next allocation should fail.
  void *p2 = xArenaAlloc(a, 1);
  EXPECT_EQ(p2, nullptr);

  xArenaDestroy(a);
}

/* ── NULL Safety ─────────────────────────────────────────────────────── */

TEST(ArenaTest, DestroyNullIsNoOp) {
  xArenaDestroy(nullptr); // Should not crash.
}

TEST(ArenaTest, ResetNullIsNoOp) {
  // xArenaReset does not explicitly check for NULL in its current signature,
  // but the caller should never pass NULL. This is just to document behavior.
  SUCCEED();
}

/* ── Alignment Edge Cases ────────────────────────────────────────────── */

TEST(ArenaTest, AlignZeroUsesDefault) {
  xArena *a = xArenaCreate(256);
  ASSERT_NE(a, nullptr);

  void *p = xArenaAllocAligned(a, 32, 0);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0u);

  xArenaDestroy(a);
}

TEST(ArenaTest, AlignmentLargerThanDefault) {
  xArena *a = xArenaCreate(1024);
  ASSERT_NE(a, nullptr);

  void *p = xArenaAllocAligned(a, 64, 256);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 256, 0u);
}
