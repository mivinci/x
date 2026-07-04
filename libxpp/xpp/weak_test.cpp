/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * weak_test.cpp - Tests for Weak<T>.
 *
 * Covers:
 *   - default ctor → null Weak, upgrade returns None
 *   - construction from Rc → weak count bumped, strong unchanged
 *   - copy / move / assignment semantics
 *   - upgrade() while strong > 0 → Some(Rc), strong bumped
 *   - upgrade() after every strong dropped → None, inner survives
 *   - Last Weak dropping when no strong → inner deallocated
 *   - Rc::downgrade(&r) equivalence with Weak(r)
 *   - Cycle break: tree-with-back-edges does not leak
 *   - sizeof invariant
 */

#include <atomic>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>
#include <xpp/allocator.h>
#include <xpp/option.h>
#include <xpp/rc.h>
#include <xpp/weak.h>

/* ── Compile-time guarantees ─────────────────────────────────────────── */

static_assert(sizeof(xpp::Weak<int>) == sizeof(int *), "Weak<T> must be sizeof(int*)");
static_assert(std::is_default_constructible<xpp::Weak<int>>::value,
              "Weak must default-construct (yields null)");
static_assert(std::is_copy_constructible<xpp::Weak<int>>::value, "Weak must be copyable");
static_assert(std::is_copy_assignable<xpp::Weak<int>>::value, "Weak must be copy-assignable");
static_assert(std::is_move_constructible<xpp::Weak<int>>::value, "Weak must be movable");
static_assert(std::is_nothrow_destructible<xpp::Weak<int>>::value,
              "Weak destructor must be noexcept");

namespace {

/* ── Tracker fixture ────────────────────────────────────────────────── */

struct Tracker {
  static int alive;
  int        value;

  Tracker() : value(0) {
    ++alive;
  }
  explicit Tracker(int v) : value(v) {
    ++alive;
  }
  Tracker(const Tracker &)            = delete;
  Tracker(Tracker &&)                 = delete;
  Tracker &operator=(const Tracker &) = delete;
  Tracker &operator=(Tracker &&)      = delete;
  ~Tracker() {
    --alive;
  }
};
int Tracker::alive = 0;

class WeakTrackerTest : public ::testing::Test {
protected:
  void SetUp() override {
    Tracker::alive = 0;
  }
  void TearDown() override {
    EXPECT_EQ(Tracker::alive, 0) << "leak: " << Tracker::alive << " Trackers still alive";
  }
};

/* ── Default ctor: null Weak ─────────────────────────────────────────── */

TEST_F(WeakTrackerTest, DefaultIsNullAndUpgradesToNone) {
  xpp::Weak<Tracker> w;
  EXPECT_TRUE(w.is_expired());
  EXPECT_EQ(w.strong_count(), 0u);
  EXPECT_EQ(w.weak_count(), 0u);
  EXPECT_TRUE(w.upgrade().is_none());
  // no Trackers ever allocated; TearDown verifies alive == 0
}

/* ── Construct from Rc: weak += 1, strong unchanged ──────────────────── */

TEST_F(WeakTrackerTest, ConstructFromRcBumpsWeak) {
  {
    xpp::Rc<Tracker> r = xpp::Rc<Tracker>::make(42);
    EXPECT_EQ(r.strong_count(), 1u);
    EXPECT_EQ(r.weak_count(), 0u) << "no Weaks yet";

    xpp::Weak<Tracker> w(r);
    EXPECT_EQ(r.strong_count(), 1u);
    EXPECT_EQ(r.weak_count(), 1u);
    EXPECT_FALSE(w.is_expired());
    EXPECT_EQ(w.strong_count(), 1u);
    EXPECT_EQ(w.weak_count(), 1u);
  } // w drops, then r drops, then Tracker destroyed
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(WeakTrackerTest, RcDowngradeEquivalent) {
  {
    xpp::Rc<Tracker>   r = xpp::Rc<Tracker>::make(7);
    xpp::Weak<Tracker> w = xpp::Rc<Tracker>::downgrade(r);
    EXPECT_EQ(r.weak_count(), 1u);
    EXPECT_FALSE(w.is_expired());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Upgrade while strong > 0 ─────────────────────────────────────────── */

TEST_F(WeakTrackerTest, UpgradeWhileStrongAliveBumpsStrong) {
  {
    xpp::Rc<Tracker>   r = xpp::Rc<Tracker>::make(11);
    xpp::Weak<Tracker> w(r);

    xpp::Option<xpp::Rc<Tracker>> upgraded = w.upgrade();
    EXPECT_TRUE(upgraded.is_some());
    EXPECT_EQ(r.strong_count(), 2u) << "upgrade should bump strong";
    EXPECT_EQ(r.weak_count(), 1u) << "weak unchanged by upgrade";

    xpp::Rc<Tracker> r2 = std::move(upgraded).unwrap();
    EXPECT_EQ(r2->value, 11);
    EXPECT_EQ(r2.get(), r.get());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Upgrade after all strongs drop returns None ─────────────────────── */

TEST_F(WeakTrackerTest, UpgradeAfterStrongsGoneReturnsNone) {
  xpp::Weak<Tracker> w;
  {
    xpp::Rc<Tracker> r = xpp::Rc<Tracker>::make(99);
    w                  = xpp::Weak<Tracker>(r);
    EXPECT_FALSE(w.is_expired());
    EXPECT_EQ(Tracker::alive, 1);
  }
  // Strong dropped: Tracker destroyed, but inner still alive because Weak holds it.
  EXPECT_EQ(Tracker::alive, 0) << "Tracker destroyed when last strong drops";
  EXPECT_TRUE(w.is_expired());
  EXPECT_EQ(w.strong_count(), 0u);
  EXPECT_TRUE(w.upgrade().is_none());
  // Now w drops at end of scope; inner is deallocated. Nothing observable to assert.
}

/* ── Last Weak drop after strong=0 deallocates inner (no leak) ───────── */

TEST_F(WeakTrackerTest, NoLeakWhenWeakOutlivesAllStrongs) {
  {
    xpp::Weak<Tracker> w;
    {
      xpp::Rc<Tracker> r = xpp::Rc<Tracker>::make(5);
      w                  = xpp::Weak<Tracker>(r);
    } // strong gone, Tracker destroyed, inner still alive
    EXPECT_EQ(Tracker::alive, 0);
    EXPECT_TRUE(w.is_expired());
  } // w gone, inner deallocated — leak-check via valgrind/asan in CI
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Copy / move of Weak ─────────────────────────────────────────────── */

TEST_F(WeakTrackerTest, WeakCopyBumpsWeakCount) {
  {
    xpp::Rc<Tracker>   r = xpp::Rc<Tracker>::make(1);
    xpp::Weak<Tracker> w1(r);
    {
      xpp::Weak<Tracker> w2 = w1;
      EXPECT_EQ(r.weak_count(), 2u);
    } // w2 dies
    EXPECT_EQ(r.weak_count(), 1u);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(WeakTrackerTest, WeakMoveDoesNotBumpWeakCount) {
  {
    xpp::Rc<Tracker>   r = xpp::Rc<Tracker>::make(2);
    xpp::Weak<Tracker> w1(r);
    EXPECT_EQ(r.weak_count(), 1u);
    xpp::Weak<Tracker> w2 = std::move(w1);
    EXPECT_EQ(r.weak_count(), 1u);
    EXPECT_FALSE(w2.is_expired());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(WeakTrackerTest, AssignWeakToNullDropsObservation) {
  {
    xpp::Rc<Tracker>   r = xpp::Rc<Tracker>::make(3);
    xpp::Weak<Tracker> w(r);
    EXPECT_EQ(r.weak_count(), 1u);
    w = xpp::Weak<Tracker>(); // null
    EXPECT_EQ(r.weak_count(), 0u);
    EXPECT_TRUE(w.is_expired());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Cycle break: list with weak back-pointer doesn't leak ───────────── */

struct CycleNode {
  static int                      alive;
  int                             value;
  xpp::Option<xpp::Rc<CycleNode>> next;
  xpp::Weak<CycleNode>            prev;

  explicit CycleNode(int v) : value(v) {
    ++alive;
  }
  ~CycleNode() {
    --alive;
  }
};
int CycleNode::alive = 0;

class CycleTest : public ::testing::Test {
protected:
  void SetUp() override {
    CycleNode::alive = 0;
  }
  void TearDown() override {
    EXPECT_EQ(CycleNode::alive, 0) << "leak: " << CycleNode::alive << " CycleNodes alive";
  }
};

TEST_F(CycleTest, ForwardRcBackwardWeakReleasesAll) {
  {
    xpp::Rc<CycleNode> a = xpp::Rc<CycleNode>::make(1);
    xpp::Rc<CycleNode> b = xpp::Rc<CycleNode>::make(2);

    a->next = xpp::Option<xpp::Rc<CycleNode>>(b); // a → b (strong)
    b->prev = xpp::Weak<CycleNode>(a);            // b ⇠ a (weak)

    EXPECT_EQ(a.strong_count(), 1u) << "Weak does not bump strong";
    EXPECT_EQ(b.strong_count(), 2u) << "a->next holds one + local b holds one";
    EXPECT_EQ(a.weak_count(), 1u);
    EXPECT_EQ(CycleNode::alive, 2);
  } // a goes out of scope → strong=0, a destroyed; b's prev becomes expired;
    // b's local handle drops, a->next is already gone with a, so b also dies.
  EXPECT_EQ(CycleNode::alive, 0) << "cycle broken cleanly";
}

// Companion negative test for ForwardRcBackwardWeakReleasesAll would
// demonstrate that an all-strong cycle leaks — but writing such a
// test would intentionally leak memory and trip ASan in CI, so we
// settle for the README's documentation of "no Weak<T> → don't build
// cycles" instead.

} // namespace

/* ── Allocator parameter tests for Weak ─────────────────────────────── */

namespace {

struct WeakCountingAlloc {
  std::atomic<int> *allocs;
  std::atomic<int> *deallocs;

  WeakCountingAlloc(std::atomic<int> *a, std::atomic<int> *d) : allocs(a), deallocs(d) {}

  xpp::Result<xpp::Span<uint8_t>, xpp::AllocError> allocate(xpp::Layout layout) const {
    void *p = ::operator new(layout.size);
    if (!p) return xpp::Result<xpp::Span<uint8_t>, xpp::AllocError>(xpp::err, xpp::AllocError{});
    allocs->fetch_add(1, std::memory_order_relaxed);
    return xpp::Result<xpp::Span<uint8_t>, xpp::AllocError>(
      xpp::ok, xpp::Span<uint8_t>(static_cast<uint8_t *>(p), layout.size));
  }

  void deallocate(void *ptr, xpp::Layout) const {
    deallocs->fetch_add(1, std::memory_order_relaxed);
    ::operator delete(ptr);
  }
};

TEST(WeakAllocTest, WeakWithStatefulAllocator) {
  std::atomic<int>  allocs{0}, deallocs{0};
  WeakCountingAlloc alloc(&allocs, &deallocs);

  // Weak outlives Rc — the trickiest path for stateful alloc: the
  // Alloc must be moved out of RcInner when the last weak drops.
  {
    xpp::Weak<int, WeakCountingAlloc> w;
    {
      xpp::Rc<int, WeakCountingAlloc> r = xpp::Rc<int, WeakCountingAlloc>::make_in(alloc, 99);
      w                                 = xpp::Rc<int, WeakCountingAlloc>::downgrade(r);
      EXPECT_EQ(r.weak_count(), 1u);

      xpp::Option<xpp::Rc<int, WeakCountingAlloc>> upgraded = w.upgrade();
      ASSERT_TRUE(upgraded.is_some());
      EXPECT_EQ(*std::move(upgraded).unwrap(), 99);
    }
    EXPECT_EQ(deallocs.load(), 0) << "inner kept alive by weak";
    EXPECT_TRUE(w.is_expired());
    EXPECT_TRUE(w.upgrade().is_none());
  }
  EXPECT_EQ(allocs.load(), 1);
  EXPECT_EQ(deallocs.load(), 1) << "freed when last weak drops";
}

TEST(WeakAllocTest, WeakCopyAndAssignWithStatefulAllocator) {
  std::atomic<int>  allocs{0}, deallocs{0};
  WeakCountingAlloc alloc(&allocs, &deallocs);

  xpp::Rc<int, WeakCountingAlloc>   r  = xpp::Rc<int, WeakCountingAlloc>::make_in(alloc, 5);
  xpp::Weak<int, WeakCountingAlloc> w1 = xpp::Rc<int, WeakCountingAlloc>::downgrade(r);
  xpp::Weak<int, WeakCountingAlloc> w2 = w1;            // copy
  xpp::Weak<int, WeakCountingAlloc> w3 = std::move(w1); // move

  EXPECT_EQ(r.weak_count(), 2u) << "w1 moved out (still alive, just null); w2+w3 alive";
  EXPECT_TRUE(w3.upgrade().is_some());
  EXPECT_TRUE(w2.upgrade().is_some());
}

} // namespace
