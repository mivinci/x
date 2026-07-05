#include <gtest/gtest.h>
#include <xpp/io/repeat.h>
#include <xpp/io/util.h>

TEST(RepeatTest, SingleByte) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  auto r  = xpp::io::repeat('X');
  char c;
  ssize_t n = r.read(&c, 1).wait();
  EXPECT_EQ(n, 1);
  EXPECT_EQ(c, 'X');
}

TEST(RepeatTest, MultiByte) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  auto r  = xpp::io::repeat('A');
  char buf[8] = {};
  ssize_t n = r.read(buf, sizeof(buf)).wait();
  EXPECT_EQ(n, 8);
  EXPECT_EQ(std::string(buf, 8), "AAAAAAAA");
}
