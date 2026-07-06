#include <gtest/gtest.h>
#include <xpp/io/split.h>
#include <xpp/net/tcp.h>
#include <xpp/net/test_helpers.h>
#include <xpp/promise.h>
#include <xpp/promise_combinators.h>

using xpp::net::TcpListener;
using xpp::net::TcpStream;

xpp::Promise<void> do_split_concurrent_read_write() {
  uint16_t port = get_free_port();
  auto     lr = co_await xpp::net::TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str());
  TcpListener listener = std::move(lr).unwrap();

  auto server = listener.accept();
  auto client = xpp::net::TcpStream::connect(("127.0.0.1:" + std::to_string(port)).c_str());

  auto [sp, cr] = co_await xpp::all(std::move(server), std::move(client));
  TcpStream sc  = std::move(sp.first);
  TcpStream cc  = std::move(cr).unwrap();

  co_await sc.write("hello", 5);

  // Split the client into reader and writer
  auto [reader, writer] = xpp::io::split(std::move(cc));

  // Read from split reader, write from split writer — concurrent
  auto wp     = writer.write("pong", 4);
  char buf[8] = {};
  auto rp     = reader.read(buf, sizeof(buf));

  auto [w, r] = co_await xpp::all(std::move(wp), std::move(rp));
  EXPECT_EQ(w, 4);
  EXPECT_EQ(r, 5);
  co_return;
}

TEST(SplitTest, ConcurrentReadWrite) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_split_concurrent_read_write().wait();
}
