/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * vec_test.cpp — Tests for Vec<T, Alloc>.
 *
 * Covers construction, push/pop, element access, capacity management,
 * resize/append/split/swap_remove/retain, iteration, copy/move semantics,
 * Option-returning accessors, and edge cases.
 */

#include <gtest/gtest.h>

#include <string>
#include <utility>

#include <xpp/vec.h>

using namespace xpp;

/* ── Trivial type tests ────────────────────────────────────────────── */

TEST(VecInt, DefaultConstruction) {
  Vec<int> v;
  EXPECT_EQ(v.len(), 0u);
  EXPECT_EQ(v.capacity(), 0u);
  EXPECT_TRUE(v.empty());
}

TEST(VecInt, PushAndAccess) {
  Vec<int> v;
  v.push(42);
  v.push(7);
  v.push(99);

  EXPECT_EQ(v.len(), 3u);
  EXPECT_EQ(v[0], 42);
  EXPECT_EQ(v[1], 7);
  EXPECT_EQ(v[2], 99);
}

TEST(VecInt, PushLvalue) {
  Vec<int> v;
  int x = 10;
  v.push(x);  // lvalue overload
  EXPECT_EQ(v[0], 10);
  x = 20;     // should not affect vec
  EXPECT_EQ(v[0], 10);
}

TEST(VecInt, Pop) {
  Vec<int> v;
  v.push(1);
  v.push(2);

  auto x = v.pop();
  ASSERT_TRUE(x.is_some());
  EXPECT_EQ(x.unwrap(), 2);
  EXPECT_EQ(v.len(), 1u);

  auto y = v.pop();
  ASSERT_TRUE(y.is_some());
  EXPECT_EQ(y.unwrap(), 1);
  EXPECT_EQ(v.len(), 0u);

  auto z = v.pop();
  EXPECT_TRUE(z.is_none());
}

TEST(VecInt, Clear) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);
  size_t cap_before = v.capacity();
  v.clear();
  EXPECT_EQ(v.len(), 0u);
  EXPECT_TRUE(v.empty());
  EXPECT_EQ(v.capacity(), cap_before); // capacity preserved
}

TEST(VecInt, Truncate) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);
  v.push(4);

  v.truncate(2);
  EXPECT_EQ(v.len(), 2u);
  EXPECT_EQ(v[0], 1);
  EXPECT_EQ(v[1], 2);

  v.truncate(10); // no-op
  EXPECT_EQ(v.len(), 2u);
}

TEST(VecInt, Get) {
  Vec<int> v;
  v.push(10);
  v.push(20);

  auto g0 = v.get(0);
  ASSERT_TRUE(g0.is_some());
  EXPECT_EQ(g0.unwrap(), 10);

  auto g1 = v.get(1);
  ASSERT_TRUE(g1.is_some());
  EXPECT_EQ(g1.unwrap(), 20);

  auto g2 = v.get(2);
  EXPECT_TRUE(g2.is_none());

  auto g99 = v.get(99);
  EXPECT_TRUE(g99.is_none());
}

TEST(VecInt, FirstLast) {
  Vec<int> v;
  EXPECT_TRUE(v.first().is_none());
  EXPECT_TRUE(v.last().is_none());

  v.push(10);
  ASSERT_TRUE(v.first().is_some());
  EXPECT_EQ(v.first().unwrap(), 10);
  ASSERT_TRUE(v.last().is_some());
  EXPECT_EQ(v.last().unwrap(), 10);

  v.push(20);
  EXPECT_EQ(v.first().unwrap(), 10);
  EXPECT_EQ(v.last().unwrap(), 20);
}

TEST(VecInt, GetConst) {
  const Vec<int> v = []() {
    Vec<int> tmp;
    tmp.push(1);
    tmp.push(2);
    return tmp;
  }();

  auto g = v.get(0);
  ASSERT_TRUE(g.is_some());
  EXPECT_EQ(g.unwrap(), 1);

  EXPECT_TRUE(v.get(99).is_none());
}

TEST(VecInt, CapacityGrowth) {
  Vec<int> v;
  EXPECT_EQ(v.capacity(), 0u);

  v.push(1);
  EXPECT_GE(v.capacity(), 1u);

  for (int i = 0; i < 1000; i++) v.push(i);
  EXPECT_EQ(v.len(), 1001u);
  EXPECT_GE(v.capacity(), 1001u);
  for (int i = 0; i < 1000; i++) EXPECT_EQ(v[i + 1], i);
}

TEST(VecInt, Reserve) {
  Vec<int> v;
  v.reserve(100);
  size_t cap = v.capacity();
  EXPECT_GE(cap, 100u);

  // Pushing within reserved capacity should not change capacity
  for (int i = 0; i < 50; i++) v.push(i);
  EXPECT_EQ(v.capacity(), cap);
}

TEST(VecInt, ReserveZero) {
  Vec<int> v;
  v.reserve(0);
  EXPECT_EQ(v.len(), 0u);
}

TEST(VecInt, ShrinkToFit) {
  Vec<int> v;
  for (int i = 0; i < 100; i++) v.push(i);

  size_t cap_before = v.capacity();
  EXPECT_GT(cap_before, v.len());

  v.shrink_to_fit();
  EXPECT_EQ(v.capacity(), v.len());

  // Data preserved
  for (size_t i = 0; i < v.len(); i++) EXPECT_EQ(v[i], static_cast<int>(i));
}

TEST(VecInt, ShrinkToFitAlreadyTight) {
  Vec<int> v;
  v.push(1);
  v.shrink_to_fit();
  EXPECT_EQ(v.capacity(), 1u);
  v.shrink_to_fit(); // no-op
  EXPECT_EQ(v.capacity(), 1u);
}

TEST(VecInt, CopyConstructor) {
  Vec<int> a;
  a.push(1);
  a.push(2);
  a.push(3);

  Vec<int> b(a);
  EXPECT_EQ(b.len(), 3u);
  EXPECT_EQ(b[0], 1);
  EXPECT_EQ(b[1], 2);
  EXPECT_EQ(b[2], 3);

  // Deep copy: modifying a does not affect b
  a[0] = 99;
  EXPECT_EQ(b[0], 1);
}

TEST(VecInt, CopyAssignment) {
  Vec<int> a;
  a.push(10);
  a.push(20);

  Vec<int> b;
  b.push(99);
  b = a;

  EXPECT_EQ(b.len(), 2u);
  EXPECT_EQ(b[0], 10);
  EXPECT_EQ(b[1], 20);
}

TEST(VecInt, MoveConstructor) {
  Vec<int> a;
  a.push(1);
  a.push(2);
  a.push(3);

  Vec<int> b(std::move(a));
  EXPECT_EQ(b.len(), 3u);
  EXPECT_EQ(b[0], 1);
  EXPECT_EQ(b[2], 3);

  // Source is empty
  EXPECT_EQ(a.len(), 0u);
  EXPECT_EQ(a.capacity(), 0u);
}

TEST(VecInt, MoveAssignment) {
  Vec<int> a;
  a.push(1);
  a.push(2);

  Vec<int> b;
  b.push(99);
  b = std::move(a);

  EXPECT_EQ(b.len(), 2u);
  EXPECT_EQ(b[0], 1);

  // Source is empty
  EXPECT_EQ(a.len(), 0u);
}

TEST(VecInt, SelfAssignment) {
  Vec<int> v;
  v.push(1);
  v.push(2);

  // Self copy
  Vec<int>& ref = v;
  v = ref;
  EXPECT_EQ(v.len(), 2u);
  EXPECT_EQ(v[0], 1);

  // Self move (pathological but should not crash)
  v = std::move(v);
  EXPECT_EQ(v.len(), 2u);
}

TEST(VecInt, ResizeGrow) {
  Vec<int> v;
  v.resize(5, 42);
  EXPECT_EQ(v.len(), 5u);
  for (size_t i = 0; i < 5; i++) EXPECT_EQ(v[i], 42);
}

TEST(VecInt, ResizeShrink) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);
  v.push(4);

  v.resize(2, 0);
  EXPECT_EQ(v.len(), 2u);
  EXPECT_EQ(v[0], 1);
  EXPECT_EQ(v[1], 2);
}

TEST(VecInt, ResizeSame) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.resize(2, 99);
  EXPECT_EQ(v.len(), 2u);
  EXPECT_EQ(v[0], 1);
  EXPECT_EQ(v[1], 2);
}

TEST(VecInt, Append) {
  Vec<int> a;
  a.push(1);
  a.push(2);

  Vec<int> b;
  b.push(3);
  b.push(4);

  a.append(b);
  EXPECT_EQ(a.len(), 4u);
  EXPECT_EQ(a[0], 1);
  EXPECT_EQ(a[1], 2);
  EXPECT_EQ(a[2], 3);
  EXPECT_EQ(a[3], 4);
  EXPECT_TRUE(b.empty()); // source consumed
}

TEST(VecInt, AppendEmpty) {
  Vec<int> a;
  a.push(1);

  Vec<int> b; // empty
  a.append(b);
  EXPECT_EQ(a.len(), 1u);
  EXPECT_EQ(a[0], 1);
}

TEST(VecInt, SplitOff) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);
  v.push(4);

  Vec<int> tail = v.split_off(2);
  EXPECT_EQ(v.len(), 2u);
  EXPECT_EQ(v[0], 1);
  EXPECT_EQ(v[1], 2);

  EXPECT_EQ(tail.len(), 2u);
  EXPECT_EQ(tail[0], 3);
  EXPECT_EQ(tail[1], 4);
}

TEST(VecInt, SplitOffAll) {
  Vec<int> v;
  v.push(1);
  v.push(2);

  Vec<int> tail = v.split_off(0);
  EXPECT_TRUE(v.empty());
  EXPECT_EQ(tail.len(), 2u);
  EXPECT_EQ(tail[0], 1);
}

TEST(VecInt, SplitOffNone) {
  Vec<int> v;
  v.push(1);
  v.push(2);

  Vec<int> tail = v.split_off(2);
  EXPECT_EQ(v.len(), 2u);
  EXPECT_TRUE(tail.empty());
}

TEST(VecInt, SwapRemove) {
  Vec<int> v;
  v.push(10);
  v.push(20);
  v.push(30);

  int x = v.swap_remove(0);
  EXPECT_EQ(x, 10);
  EXPECT_EQ(v.len(), 2u);
  // Order not preserved, but last element moved to index 0
  EXPECT_EQ(v[0], 30);
}

TEST(VecInt, SwapRemoveLast) {
  Vec<int> v;
  v.push(10);
  v.push(20);

  int x = v.swap_remove(1);
  EXPECT_EQ(x, 20);
  EXPECT_EQ(v.len(), 1u);
  EXPECT_EQ(v[0], 10);
}

TEST(VecInt, Retain) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);
  v.push(4);
  v.push(5);
  v.push(6);

  v.retain([](int x) { return x % 2 == 0; });
  EXPECT_EQ(v.len(), 3u);
  EXPECT_EQ(v[0], 2);
  EXPECT_EQ(v[1], 4);
  EXPECT_EQ(v[2], 6);
}

TEST(VecInt, RetainAll) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.retain([](int) { return true; });
  EXPECT_EQ(v.len(), 2u);
}

TEST(VecInt, RetainNone) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.retain([](int) { return false; });
  EXPECT_TRUE(v.empty());
}

TEST(VecInt, PushUnchecked) {
  Vec<int> v;
  v.reserve(10);
  size_t cap = v.capacity();
  for (int i = 0; i < static_cast<int>(cap); i++) {
    v.push_unchecked(std::move(i));
  }
  EXPECT_EQ(v.len(), cap);
}

TEST(VecInt, Iteration) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);

  int sum = 0;
  for (auto& x : v) sum += x;
  EXPECT_EQ(sum, 6);
}

TEST(VecInt, ConstIteration) {
  const Vec<int> v = []() {
    Vec<int> tmp;
    tmp.push(10);
    tmp.push(20);
    return tmp;
  }();

  int sum = 0;
  for (const auto& x : v) sum += x;
  EXPECT_EQ(sum, 30);
}

TEST(VecInt, AsSpan) {
  Vec<int> v;
  v.push(1);
  v.push(2);

  Span<int> s = v.as_span();
  EXPECT_EQ(s.size(), 2u);
  EXPECT_EQ(s[0], 1);

  Span<const int> cs = static_cast<const Vec<int>&>(v).as_span();
  EXPECT_EQ(cs.size(), 2u);
}

TEST(VecInt, Data) {
  Vec<int> v;
  v.push(42);
  EXPECT_EQ(*v.data(), 42);
  EXPECT_EQ(v.data(), &v[0]);
}

/* ── Non-trivial type tests ────────────────────────────────────────── */

struct Tracker {
  static int alive;
  int value;

  Tracker(int v) : value(v) { ++alive; }
  Tracker(const Tracker& o) : value(o.value) { ++alive; }
  Tracker(Tracker&& o) noexcept : value(o.value) { o.value = -1; ++alive; }
  ~Tracker() { --alive; }

  Tracker& operator=(const Tracker& o) { value = o.value; return *this; }
  Tracker& operator=(Tracker&& o) noexcept { value = o.value; o.value = -1; return *this; }
};
int Tracker::alive = 0;

TEST(VecTracker, DestructorCleansUp) {
  EXPECT_EQ(Tracker::alive, 0);
  {
    Vec<Tracker> v;
    v.push(Tracker(1));
    v.push(Tracker(2));
    v.push(Tracker(3));
    EXPECT_EQ(Tracker::alive, 3);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(VecTracker, PopDestroys) {
  Vec<Tracker> v;
  v.push(Tracker(10));
  v.push(Tracker(20));

  EXPECT_EQ(Tracker::alive, 2);
  auto x = v.pop();
  EXPECT_EQ(Tracker::alive, 2); // moved into Option
  x = none;                     // destroy
  EXPECT_EQ(Tracker::alive, 1);
}

TEST(VecTracker, ClearDestroys) {
  Vec<Tracker> v;
  v.push(Tracker(1));
  v.push(Tracker(2));
  EXPECT_EQ(Tracker::alive, 2);
  v.clear();
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(VecTracker, TruncateDestroys) {
  Vec<Tracker> v;
  v.push(Tracker(1));
  v.push(Tracker(2));
  v.push(Tracker(3));
  EXPECT_EQ(Tracker::alive, 3);
  v.truncate(1);
  EXPECT_EQ(Tracker::alive, 1);
}

TEST(VecTracker, MoveConstructorTransfers) {
  Vec<Tracker> a;
  a.push(Tracker(1));
  a.push(Tracker(2));
  EXPECT_EQ(Tracker::alive, 2);

  Vec<Tracker> b(std::move(a));
  EXPECT_EQ(Tracker::alive, 2); // still 2, moved not copied
  EXPECT_EQ(a.len(), 0u);
}

TEST(VecTracker, MoveAssignmentCleansUp) {
  Vec<Tracker> a;
  a.push(Tracker(1));

  Vec<Tracker> b;
  b.push(Tracker(10));
  b.push(Tracker(20));
  EXPECT_EQ(Tracker::alive, 3);

  b = std::move(a);
  EXPECT_EQ(Tracker::alive, 1); // b's old elements destroyed
}

TEST(VecTracker, RetainDestroys) {
  Vec<Tracker> v;
  v.push(Tracker(1));
  v.push(Tracker(2));
  v.push(Tracker(3));
  EXPECT_EQ(Tracker::alive, 3);
  v.retain([](const Tracker& t) { return t.value % 2 == 0; });
  EXPECT_EQ(Tracker::alive, 1);
  EXPECT_EQ(v[0].value, 2);
}

/* ── Custom allocator test ─────────────────────────────────────────── */

struct CountingAlloc {
  mutable size_t allocs = 0;
  mutable size_t frees  = 0;

  Result<Span<uint8_t>, AllocError> allocate(Layout layout) const {
    void* p = ::operator new(layout.size, std::nothrow);
    if (!p) return Result<Span<uint8_t>, AllocError>(err, AllocError{});
    ++allocs;
    return Result<Span<uint8_t>, AllocError>(ok, Span<uint8_t>(static_cast<uint8_t*>(p), layout.size));
  }

  void deallocate(void* ptr, Layout layout) const {
    ::operator delete(ptr);
    ++frees;
    (void)layout;
  }
};

TEST(VecCustomAlloc, UsesProvidedAllocator) {
  CountingAlloc ca;
  {
    Vec<int, CountingAlloc> v(ca);
    v.push(1);
    v.push(2);
    EXPECT_GT(v.allocator().allocs, 0u);
    // Capture frees before Vec is destroyed (allocator is a copy inside v)
    size_t frees_before = v.allocator().frees;
  }
  // Original ca was never used — the Vec made a copy
}

TEST(VecCustomAlloc, CapacityConstructorUsesAlloc) {
  CountingAlloc ca;
  {
    Vec<int, CountingAlloc> v(10, ca);
    EXPECT_GE(v.capacity(), 10u);
    EXPECT_GT(v.allocator().allocs, 0u);
  }
}

/* ── Empty / edge case tests ──────────────────────────────────────── */

TEST(VecEdge, PopEmpty) {
  Vec<int> v;
  EXPECT_TRUE(v.pop().is_none());
  EXPECT_TRUE(v.pop().is_none()); // idempotent
}

TEST(VecEdge, FirstLastEmpty) {
  Vec<int> v;
  EXPECT_TRUE(v.first().is_none());
  EXPECT_TRUE(v.last().is_none());
}

TEST(VecEdge, GetEmpty) {
  Vec<int> v;
  EXPECT_TRUE(v.get(0).is_none());
}

TEST(VecEdge, SplitOffEmpty) {
  Vec<int> v;
  Vec<int> tail = v.split_off(0);
  EXPECT_TRUE(v.empty());
  EXPECT_TRUE(tail.empty());
}

TEST(VecEdge, CopyEmpty) {
  Vec<int> a;
  Vec<int> b(a);
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.capacity(), 0u);
}

TEST(VecEdge, MoveEmpty) {
  Vec<int> a;
  Vec<int> b(std::move(a));
  EXPECT_TRUE(b.empty());
}

TEST(VecEdge, AppendToEmpty) {
  Vec<int> a;
  Vec<int> b;
  b.push(1);
  b.push(2);
  a.append(b);
  EXPECT_EQ(a.len(), 2u);
  EXPECT_TRUE(b.empty());
}

TEST(VecEdge, CapacityConstructorZero) {
  Vec<int> v(0);
  EXPECT_EQ(v.len(), 0u);
  // Should not have allocated
}

TEST(VecEdge, LargeSplitOff) {
  Vec<int> v;
  for (int i = 0; i < 500; i++) v.push(i);

  Vec<int> tail = v.split_off(250);
  EXPECT_EQ(v.len(), 250u);
  EXPECT_EQ(tail.len(), 250u);
  EXPECT_EQ(v[249], 249);
  EXPECT_EQ(tail[0], 250);
}

TEST(VecEdge, SwapRemoveSingleElement) {
  Vec<int> v;
  v.push(42);
  int x = v.swap_remove(0);
  EXPECT_EQ(x, 42);
  EXPECT_TRUE(v.empty());
}
