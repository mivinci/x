/*
 * mutex_test.cpp — Prototype test for xpp::loom::Mutex<T>.
 */
#include <gtest/gtest.h>
#include <xpp/loom/mutex.h>

// ── Basic lock/unlock ────────────────────────────────────────────────

TEST(MutexTest, BasicLock) {
  xpp::loom::Mutex<int> m(0);
  {
    auto g = m.lock();
    *g = 42;
  }
  auto g2 = m.lock();
  EXPECT_EQ(*g2, 42);
}

// ── Struct data ──────────────────────────────────────────────────────

struct Foo {
  int x;
  int y;
};

TEST(MutexTest, StructData) {
  xpp::loom::Mutex<Foo> m(Foo{1, 2});
  {
    auto g = m.lock();
    g->x = 42;
    g->y = 99;
  }
  auto g = m.lock();
  EXPECT_EQ(g->x, 42);
  EXPECT_EQ(g->y, 99);
}

// ── move-only guard ──────────────────────────────────────────────────

TEST(MutexTest, GuardIsMoveOnly) {
  xpp::loom::Mutex<int> m(0);
  auto g = m.lock();
  *g     = 7;
  auto g2 = std::move(g); // move to another guard
  EXPECT_EQ(*g2, 7);
}
