#include <gtest/gtest.h>
#include <xpp/io/join.h>
#include <xpp/io/simplex.h>
#include <xpp/io/utils.h>

TEST(JoinTest, CombineSimplex) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [reader, writer] = xpp::io::simplex(256);
  auto joined           = xpp::io::join(std::move(reader), std::move(writer));

  ssize_t nw = joined.write("hi", 2).await();
  EXPECT_EQ(nw, 2);

  char    buf[4];
  ssize_t nr = joined.read(buf, sizeof(buf)).await();
  EXPECT_EQ(nr, 2);
  EXPECT_EQ(std::string(buf, 2), "hi");
}
