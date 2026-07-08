/*
 * server_test.cpp — Tests for xpp::http server module.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <xpp/event.h>
#include <xpp/http/server.h>

#include <x/http/client.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

static uint16_t find_free_port() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  struct sockaddr_in addr = {};
  addr.sin_family         = AF_INET;
  addr.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
  addr.sin_port           = 0;
  if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len);
  uint16_t port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

/* ── Client helpers ───────────────────────────────────────────────── */

struct ClientCtx {
  std::string       body;
  std::atomic<bool> done{false};
};

static int client_on_data(const char *data, size_t len, void *arg) {
  static_cast<ClientCtx *>(arg)->body.append(data, len);
  return 0;
}

static void client_on_done(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<ClientCtx *>(arg);
  c->done.store(true);
}

static void run_until(xEventLoop loop, std::atomic<bool> &done, int timeout_ms = 5000) {
  for (int i = 0; i < timeout_ms / 10 && !done.load(); i++) {
    xEventLoopRun(loop, X_RUN_ONCE);
  }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Unit tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(ServerTest, CreateRouter) {
  xpp::http::Router router;
  SUCCEED();
}

TEST(ServerTest, RouterMove) {
  xpp::http::Router a;
  a.route("/hello", [](xpp::http::Request &) {});
  xpp::http::Router b = std::move(a);
  SUCCEED();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Integration tests (server + client)
 * ═══════════════════════════════════════════════════════════════════ */

class ServerIntegrationTest : public ::testing::Test {
protected:
  xEventLoop         m_loop   = nullptr;
  xpp::http::Server  m_server;
  uint16_t           m_port   = 0;

  void SetUp() override {
    m_loop = xEventLoopCreate();
    ASSERT_NE(m_loop, nullptr);
    xEventLoopEnter(m_loop);

    m_port = find_free_port();
    ASSERT_NE(m_port, 0);
  }

  void TearDown() override {
    // Destroy server first (OwnedHandle destructor cleans up xHttpServer)
    m_server = {};
    if (m_loop) {
      xEventLoopLeave();
      xEventLoopDestroy(m_loop);
    }
  }

  void serve(xpp::http::Router &router) {
    auto addr     = std::string("127.0.0.1:") + std::to_string(m_port);
    auto result   = xpp::http::Server::bind(addr.c_str(), router);
    ASSERT_TRUE(result.is_ok()) << "bind failed";
    m_server      = std::move(result).unwrap();
    m_server.listen();

    // Pump the event loop to accept connections
    for (int i = 0; i < 20; i++)
      xEventLoopRun(m_loop, X_RUN_ONCE);
  }

  std::string url(const char *path) const {
    return "http://127.0.0.1:" + std::to_string(m_port) + path;
  }
};

TEST_F(ServerIntegrationTest, HelloWorld) {
  xpp::http::Router router;
  router.route("GET /hello", [](xpp::http::Request &req) {
    EXPECT_EQ(req.method(), "GET");
    EXPECT_EQ(req.path(), "/hello");
    req.respond().status(200).body("Hello from xpp!");
  });
  serve(router);

  // Send a client request
  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  ClientCtx ctx;
  auto      url_str = url("/hello");
  xHttpRequestConf conf = {};
  conf.url      = url_str.c_str();
  conf.method   = xHttpMethod_GET;
  conf.on_data  = client_on_data;
  conf.on_done  = client_on_done;
  ASSERT_EQ(xHttpClientDo(client, &conf, &ctx), xErrno_Ok);

  run_until(m_loop, ctx.done);
  EXPECT_TRUE(ctx.done.load());
  EXPECT_EQ(ctx.body, "Hello from xpp!");

  xHttpClientDestroy(client);
}

TEST_F(ServerIntegrationTest, StatusAndHeaders) {
  xpp::http::Router router;
  router.route("GET /json", [](xpp::http::Request &req) {
    req.respond()
      .status(201)
      .header("Content-Type", "application/json")
      .header("X-Custom", "v1")
      .body("{}");
  });
  serve(router);

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  ClientCtx ctx;
  auto      url_str = url("/json");
  xHttpRequestConf conf = {};
  conf.url      = url_str.c_str();
  conf.method   = xHttpMethod_GET;
  conf.on_data  = client_on_data;
  conf.on_done  = client_on_done;
  ASSERT_EQ(xHttpClientDo(client, &conf, &ctx), xErrno_Ok);

  run_until(m_loop, ctx.done);
  EXPECT_TRUE(ctx.done.load());
  EXPECT_EQ(ctx.body, "{}");

  xHttpClientDestroy(client);
}

TEST_F(ServerIntegrationTest, RouteParams) {
  xpp::http::Router router;
  router.route("GET /users/:id", [](xpp::http::Request &req) {
    auto id = req.param("id");
    req.respond().status(200).body("user:" + id);
  });
  serve(router);

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  ClientCtx ctx;
  auto      url_str = url("/users/42");
  xHttpRequestConf conf = {};
  conf.url      = url_str.c_str();
  conf.method   = xHttpMethod_GET;
  conf.on_data  = client_on_data;
  conf.on_done  = client_on_done;
  ASSERT_EQ(xHttpClientDo(client, &conf, &ctx), xErrno_Ok);

  run_until(m_loop, ctx.done);
  EXPECT_TRUE(ctx.done.load());
  EXPECT_EQ(ctx.body, "user:42");

  xHttpClientDestroy(client);
}
