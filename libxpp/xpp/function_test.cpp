/*
 * function_test.cpp — Test suite for xpp::FnOnce.
 */

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <xpp/function.h>

using namespace xpp;

/* ── Move-only callable for testing ───────────────────────────────── */

struct MoveOnlyCallable {
  int value;
  bool moved_from = false;

  explicit MoveOnlyCallable(int v) : value(v) {}
  MoveOnlyCallable(MoveOnlyCallable &&other) noexcept
    : value(other.value), moved_from(false) {
    other.moved_from = true;
  }
  MoveOnlyCallable &operator=(MoveOnlyCallable &&) = delete;
  MoveOnlyCallable(const MoveOnlyCallable &)        = delete;

  int operator()() const { return value; }
};

/* ── Large callable (> 32 bytes — beyond hypothetical SBO) ────────── */

struct LargeCallable {
  char              padding[128] = {};
  std::vector<char> captured;

  explicit LargeCallable(std::vector<char> v) : captured(std::move(v)) {}

  std::string operator()() const {
    return std::string(captured.begin(), captured.end());
  }
};

/* ═══════════════════════════════════════════════════════════════════
 *  Construction
 * ══════════════════════════════════════════════════════════════════ */

TEST(FnOnceTest, DefaultConstructionIsNone) {
  FnOnce<int()> f;
  EXPECT_TRUE(f.is_none());
  EXPECT_FALSE(f.is_some());
}

TEST(FnOnceTest, ConstructWithLambdaIsSome) {
  auto f = FnOnce<int()>([] { return 42; });
  EXPECT_FALSE(f.is_none());
  EXPECT_TRUE(f.is_some());
  int r = std::move(f)();
  EXPECT_EQ(r, 42);
}

TEST(FnOnceTest, ConstructWithFunctionPointerWorks) {
  auto f = FnOnce<int()>(static_cast<int (*)()>([]() -> int { return 7; }));
  EXPECT_TRUE(f.is_some());
  int r = std::move(f)();
  EXPECT_EQ(r, 7);
}

TEST(FnOnceTest, ConstructWithMutableLambdaWorks) {
  auto f = FnOnce<int()>([n = 0]() mutable { return ++n; });
  int  r = std::move(f)();
  EXPECT_EQ(r, 1);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Invocation — return value
 * ══════════════════════════════════════════════════════════════════ */

TEST(FnOnceTest, InvokeReturnsValue) {
  auto f = FnOnce<int()>([] { return 99; });
  int  r = std::move(f)();
  EXPECT_EQ(r, 99);
}

TEST(FnOnceTest, InvokeWithArgument) {
  auto f = FnOnce<int(int)>([](int x) { return x * 2; });
  int  r = std::move(f)(21);
  EXPECT_EQ(r, 42);
}

TEST(FnOnceTest, InvokeWithMultipleArguments) {
  auto f = FnOnce<int(int, int)>([](int a, int b) { return a + b; });
  int  r = std::move(f)(10, 20);
  EXPECT_EQ(r, 30);
}

TEST(FnOnceTest, InvokeReturnsVoidIsOk) {
  int  side   = 0;
  auto f      = FnOnce<void()>([&side] { side = 1; });
  std::move(f)();
  EXPECT_EQ(side, 1);
}

TEST(FnOnceTest, InvokeConsumesCallable) {
  auto f = FnOnce<int()>([] { return 1; });
  EXPECT_TRUE(f.is_some());
  std::move(f)();
  EXPECT_TRUE(f.is_none());
  EXPECT_FALSE(f.is_some());
}

/* ═══════════════════════════════════════════════════════════════════
 *  Invocation — side effects & captures
 * ══════════════════════════════════════════════════════════════════ */

TEST(FnOnceTest, CaptureByValue) {
  int  n   = 10;
  auto f   = FnOnce<int()>([n] { return n * n; });
  int  r   = std::move(f)();
  EXPECT_EQ(r, 100);
}

TEST(FnOnceTest, CaptureByMoveOwnsResource) {
  /* Move-only object captured — verifies heap storage ownership. */
  MoveOnlyCallable mc{42};
  auto             f = FnOnce<int()>(std::move(mc));
  EXPECT_TRUE(mc.moved_from);
  int r = std::move(f)();
  EXPECT_EQ(r, 42);
}

TEST(FnOnceTest, CaptureLargeObject) {
  std::vector<char> data(1024, 'x');
  auto              f = FnOnce<std::string()>(LargeCallable(std::move(data)));
  std::string       r = std::move(f)();
  EXPECT_EQ(r.size(), 1024u);
  EXPECT_EQ(r[0], 'x');
}

TEST(FnOnceTest, CaptureByReferenceIsFine) {
  int  n   = 5;
  auto f   = FnOnce<int()>([&n] { return n + 1; });
  int  r   = std::move(f)();
  EXPECT_EQ(r, 6);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Move semantics
 * ══════════════════════════════════════════════════════════════════ */

TEST(FnOnceTest, MoveConstructionTransfersOwnership) {
  auto src = FnOnce<int()>([] { return 42; });
  EXPECT_TRUE(src.is_some());

  auto dst = std::move(src);
  EXPECT_TRUE(dst.is_some());
  EXPECT_TRUE(src.is_none());

  int r = std::move(dst)();
  EXPECT_EQ(r, 42);
}

TEST(FnOnceTest, MoveAssignmentTransfersOwnership) {
  auto a = FnOnce<int()>([] { return 1; });
  auto b = FnOnce<int()>([] { return 2; });

  a = std::move(b);
  EXPECT_TRUE(a.is_some());
  EXPECT_TRUE(b.is_none());

  int r = std::move(a)();
  EXPECT_EQ(r, 2);
}

TEST(FnOnceTest, MoveAssignmentCleansOldCallable) {
  int cleanup_count_a = 0;
  int cleanup_count_b = 0;

  struct CountingCallable {
    int &count;
    explicit CountingCallable(int &c) : count(c) {}
    ~CountingCallable() { count++; }
    void operator()() const {}
  };

  /* Build a on the heap, then adopt into FnOnce — avoids the temporary
   * destructor inflating the count. */
  auto a = FnOnce<void()>(CountingCallable(cleanup_count_a));
  auto b = FnOnce<void()>(CountingCallable(cleanup_count_b));

  /* The temporary CountingCallable was destroyed after move-construction
   * into FnOnce — expect one destructor call for each. */
  EXPECT_EQ(cleanup_count_a, 1);
  EXPECT_EQ(cleanup_count_b, 1);

  /* Move b into a — a's old callable must be cleaned up during assignment. */
  a = std::move(b);
  EXPECT_EQ(cleanup_count_a, 2);    /* old a destroyed */
  EXPECT_EQ(cleanup_count_b, 1);    /* b's original still alive inside a */

  /* Consume a (the one hosting b's callable). */
  std::move(a)();
  EXPECT_EQ(cleanup_count_b, 2);    /* b's callable consumed */

  /* left-over from counting of temporary — b's original in a was cleaned */
}

TEST(FnOnceTest, MoveFromConsumedBecomesNone) {
  auto f = FnOnce<int()>([] { return 0; });
  std::move(f)();
  EXPECT_TRUE(f.is_none());

  auto g = std::move(f);
  EXPECT_TRUE(g.is_none());
}

/* ═══════════════════════════════════════════════════════════════════
 *  Size and layout
 * ══════════════════════════════════════════════════════════════════ */

TEST(FnOnceTest, SizeIsThreePointers) {
  EXPECT_EQ(sizeof(FnOnce<void()>), 3 * sizeof(void *));
}

TEST(FnOnceTest, MoveIsNoexcept) {
  EXPECT_TRUE(std::is_nothrow_move_constructible<FnOnce<int()>>::value);
  EXPECT_TRUE(std::is_nothrow_move_assignable<FnOnce<int()>>::value);
}

TEST(FnOnceTest, CopyIsDeleted) {
  EXPECT_FALSE(std::is_copy_constructible<FnOnce<int()>>::value);
  EXPECT_FALSE(std::is_copy_assignable<FnOnce<int()>>::value);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Death / runtime assert
 * ══════════════════════════════════════════════════════════════════ */

TEST(FnOnceTest, DoubleInvokeAsserts) {
  auto f = FnOnce<int()>([] { return 0; });
  std::move(f)();
  EXPECT_DEATH(std::move(f)(), "already consumed");
}

TEST(FnOnceTest, InvokeDefaultConstructedAsserts) {
  FnOnce<int()> f;
  EXPECT_DEATH(std::move(f)(), "default-constructed");
}

TEST(FnOnceTest, DestroyWithoutCallingAsserts) {
  EXPECT_DEATH(
    {
      auto f = FnOnce<int()>([] { return 0; });
      /* f destroyed without being called → assert */
    },
    "without being called");
}

TEST(FnOnceTest, MoveAfterInvokeIsFine) {
  auto f = FnOnce<int()>([] { return 0; });
  std::move(f)();
  EXPECT_TRUE(f.is_none());
  /* Move from consumed state: no assert, just remains none. */
  auto g = std::move(f);
  EXPECT_TRUE(g.is_none());
}
