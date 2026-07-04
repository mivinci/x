/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * allocator_test.cpp — Tests for Allocator protocol and GlobalAllocator.
 */
#include <cstring>
#include <atomic>

#include <gtest/gtest.h>
#include <xpp/allocator.h>
#include <xpp/span.h>

using namespace xpp;

/* ───────────────────── Layout ───────────────────── */

TEST(AllocatorTest, LayoutOf) {
  auto l = Layout::of<int>();
  EXPECT_EQ(l.size, sizeof(int));
  EXPECT_EQ(l.align, alignof(int));
}

TEST(AllocatorTest, LayoutOfStruct) {
  struct Big { char data[128]; };
  auto l = Layout::of<Big>();
  EXPECT_EQ(l.size, 128u);
  EXPECT_EQ(l.align, alignof(Big));
}

TEST(AllocatorTest, LayoutArray) {
  auto l = Layout::array(100, 8);
  EXPECT_EQ(l.size, 100u);
  EXPECT_EQ(l.align, 8u);
}

TEST(AllocatorTest, LayoutEquality) {
  EXPECT_TRUE(Layout::of<int>() == Layout::of<int>());
  EXPECT_FALSE(Layout::of<int>() == Layout::of<long long>());
}

/* ───────────────────── GlobalAllocator ───────────────────── */

TEST(AllocatorTest, GlobalAllocBasic) {
  GlobalAllocator alloc;
  auto r = alloc.allocate(Layout::of<int>());
  ASSERT_TRUE(r.is_ok());
  ASSERT_NE(r.unwrap().data(), nullptr);
  EXPECT_GE(r.unwrap().size(), sizeof(int));

  *reinterpret_cast<int*>(r.unwrap().data()) = 42;
  EXPECT_EQ(*reinterpret_cast<int*>(r.unwrap().data()), 42);

  alloc.deallocate(r.unwrap().data(), Layout::of<int>());
}

TEST(AllocatorTest, GlobalAllocFailure) {
  GlobalAllocator alloc;
  // Request absurdly large allocation
  auto r = alloc.allocate(Layout{SIZE_MAX, 1});
  EXPECT_TRUE(r.is_err());
}

TEST(AllocatorTest, GlobalAllocEmpty) {
  GlobalAllocator alloc;
  auto r = alloc.allocate(Layout{1, 1});
  ASSERT_TRUE(r.is_ok());
  alloc.deallocate(r.unwrap().data(), Layout{1, 1});
}

/* ───────────────────── grow / shrink ───────────────────── */

TEST(AllocatorTest, DefaultGrow) {
  GlobalAllocator alloc;
  auto r1 = alloc.allocate(Layout{4, 4});
  ASSERT_TRUE(r1.is_ok());
  *reinterpret_cast<int*>(r1.unwrap().data()) = 42;

  auto r2 = default_grow(alloc, r1.unwrap().data(), Layout{4, 4}, Layout{8, 4});
  ASSERT_TRUE(r2.is_ok());
  EXPECT_EQ(*reinterpret_cast<int*>(r2.unwrap().data()), 42);
  EXPECT_GE(r2.unwrap().size(), 8u);

  alloc.deallocate(r2.unwrap().data(), Layout{8, 4});
}

TEST(AllocatorTest, DefaultShrink) {
  GlobalAllocator alloc;
  auto r1 = alloc.allocate(Layout{8, 4});
  ASSERT_TRUE(r1.is_ok());
  std::memcpy(r1.unwrap().data(), "ABCDEFGH", 8);

  auto r2 = default_shrink(alloc, r1.unwrap().data(), Layout{8, 4}, Layout{4, 4});
  ASSERT_TRUE(r2.is_ok());
  EXPECT_EQ(std::memcmp(r2.unwrap().data(), "ABCD", 4), 0);

  alloc.deallocate(r2.unwrap().data(), Layout{4, 4});
}

/* ───────────────────── EBO ───────────────────── */

TEST(AllocatorTest, GlobalAllocatorIsEmpty) {
  EXPECT_TRUE(std::is_empty<GlobalAllocator>::value);
  EXPECT_EQ(sizeof(GlobalAllocator), 1u);  // empty class is 1 byte
}

/* ───────────────────── CountingAllocator (stateful) ───────────────────── */

struct CountingAllocator {
  std::atomic<int> *alloc_count;
  std::atomic<int> *dealloc_count;

  CountingAllocator(std::atomic<int> *a, std::atomic<int> *d)
      : alloc_count(a), dealloc_count(d) {}

  Result<Span<uint8_t>, AllocError> allocate(Layout layout) {
    void *p = ::operator new(layout.size);
    if (!p) return Result<Span<uint8_t>, AllocError>(err, AllocError{});
    alloc_count->fetch_add(1);
    return Result<Span<uint8_t>, AllocError>(ok, Span<uint8_t>(static_cast<uint8_t*>(p), layout.size));
  }

  void deallocate(void *ptr, Layout) {
    dealloc_count->fetch_add(1);
    ::operator delete(ptr);
  }
};

TEST(AllocatorTest, CountingAllocatorBasic) {
  std::atomic<int> allocs{0}, deallocs{0};
  CountingAllocator alloc(&allocs, &deallocs);

  auto r = alloc.allocate(Layout::of<int>());
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(allocs.load(), 1);

  alloc.deallocate(r.unwrap().data(), Layout::of<int>());
  EXPECT_EQ(deallocs.load(), 1);
}

TEST(AllocatorTest, CountingAllocatorGrow) {
  std::atomic<int> allocs{0}, deallocs{0};
  CountingAllocator alloc(&allocs, &deallocs);

  auto r1 = alloc.allocate(Layout{4, 4});
  ASSERT_TRUE(r1.is_ok());
  *reinterpret_cast<int*>(r1.unwrap().data()) = 99;

  auto r2 = default_grow(alloc, r1.unwrap().data(), Layout{4, 4}, Layout{8, 4});
  ASSERT_TRUE(r2.is_ok());
  EXPECT_EQ(*reinterpret_cast<int*>(r2.unwrap().data()), 99);
  EXPECT_EQ(allocs.load(), 2);   // original + grow
  EXPECT_EQ(deallocs.load(), 1); // original freed by grow

  alloc.deallocate(r2.unwrap().data(), Layout{8, 4});
  EXPECT_EQ(allocs.load(), 2);
  EXPECT_EQ(deallocs.load(), 2);
}
