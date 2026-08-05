/*
 * list_test.cpp — Tests for xpp::sync::list (lock-free bounded MPSC queue).
 */
#include <gtest/gtest.h>
#include <xpp/sync/list.h>

TEST(ListTest, PushPop) {
  auto [tx, rx] = xpp::sync::list::channel<int>(4);

  int v1 = 10, v2 = 20, v3 = 30;
  EXPECT_TRUE(tx.try_push(v1));
  EXPECT_TRUE(tx.try_push(v2));
  EXPECT_TRUE(tx.try_push(v3));

  auto r1 = rx.try_pop();
  ASSERT_TRUE(r1.is_some());
  EXPECT_EQ(r1.unwrap(), 10);

  auto r2 = rx.try_pop();
  ASSERT_TRUE(r2.is_some());
  EXPECT_EQ(r2.unwrap(), 20);

  auto r3 = rx.try_pop();
  ASSERT_TRUE(r3.is_some());
  EXPECT_EQ(r3.unwrap(), 30);

  EXPECT_TRUE(rx.try_pop().is_none());
}

TEST(ListTest, Full) {
  auto [tx, rx] = xpp::sync::list::channel<int>(2);

  int v1 = 1, v2 = 2, v3 = 3, v4 = 4;
  EXPECT_TRUE(tx.try_push(v1));
  EXPECT_TRUE(tx.try_push(v2));
  EXPECT_FALSE(tx.try_push(v3));  // full, v3 untouched

  rx.try_pop();
  EXPECT_TRUE(tx.try_push(v4));   // now has room
}

TEST(ListTest, MultipleCalls) {
  auto [tx, rx] = xpp::sync::list::channel<int>(8);

  for (int i = 0; i < 8; ++i) {
    int v = i;
    EXPECT_TRUE(tx.try_push(v));
  }
  int extra = 999;
  EXPECT_FALSE(tx.try_push(extra));  // full, value untouched

  int sum = 0;
  for (int i = 0; i < 8; ++i) {
    auto v = rx.try_pop();
    ASSERT_TRUE(v.is_some());
    sum += v.unwrap();
  }
  EXPECT_EQ(sum, 28);
}
