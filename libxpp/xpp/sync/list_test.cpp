/*
 * list_test.cpp — Tests for xpp::sync::list (lock-free bounded MPSC queue).
 */
#include <gtest/gtest.h>
#include <xpp/sync/list.h>

TEST(ListTest, PushPop) {
  typedef xpp::sync::list::Tx<int> Tx;
  typedef xpp::sync::list::Rx<int> Rx;

  std::pair<Tx, Rx> p = xpp::sync::list::channel<int>(4);
  Tx &tx = p.first;
  Rx &rx = p.second;

  EXPECT_TRUE(tx.try_push(10));
  EXPECT_TRUE(tx.try_push(20));
  EXPECT_TRUE(tx.try_push(30));

  xpp::Option<int> v1 = rx.try_pop();
  ASSERT_TRUE(v1.is_some());
  EXPECT_EQ(v1.unwrap(), 10);

  xpp::Option<int> v2 = rx.try_pop();
  ASSERT_TRUE(v2.is_some());
  EXPECT_EQ(v2.unwrap(), 20);

  xpp::Option<int> v3 = rx.try_pop();
  ASSERT_TRUE(v3.is_some());
  EXPECT_EQ(v3.unwrap(), 30);

  EXPECT_TRUE(rx.try_pop().is_none());
}

TEST(ListTest, Full) {
  typedef xpp::sync::list::Tx<int> Tx;
  typedef xpp::sync::list::Rx<int> Rx;

  std::pair<Tx, Rx> p = xpp::sync::list::channel<int>(2);
  Tx &tx = p.first;
  Rx &rx = p.second;

  EXPECT_TRUE(tx.try_push(1));
  EXPECT_TRUE(tx.try_push(2));
  EXPECT_FALSE(tx.try_push(3));

  rx.try_pop();
  EXPECT_TRUE(tx.try_push(4));
}

TEST(ListTest, MultipleCalls) {
  typedef xpp::sync::list::Tx<int> Tx;
  typedef xpp::sync::list::Rx<int> Rx;

  std::pair<Tx, Rx> p = xpp::sync::list::channel<int>(8);
  Tx &tx = p.first;
  Rx &rx = p.second;

  for (int i = 0; i < 8; ++i) EXPECT_TRUE(tx.try_push(i));
  EXPECT_FALSE(tx.try_push(999));

  int sum = 0;
  for (int i = 0; i < 8; ++i) {
    xpp::Option<int> v = rx.try_pop();
    ASSERT_TRUE(v.is_some());
    sum += v.unwrap();
  }
  EXPECT_EQ(sum, 28);
}
