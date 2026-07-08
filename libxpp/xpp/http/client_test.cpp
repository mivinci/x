/*
 * client_test.cpp — Tests for xpp::http::Client.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include <gtest/gtest.h>
#include <xpp/bytes/bytes.h>
#include <xpp/event.h>
#include <xpp/http/client.h>
#include <xpp/promise.h>
#include <xpp/sync/mpsc.h>

#include <x/http/server.h>

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

/* ── Server handler callbacks ─────────────────────────────────────── */

static void hello_handler(xHttpCtx *ctx, void * /*arg*/) {
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  xHttpCtxSend(ctx, "Hello, xpp!", 11);
}

static void chunked_handler(xHttpCtx *ctx, void * /*arg*/) {
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  xHttpCtxWrite(ctx, "chunk-1 ", 8);
  xHttpCtxWrite(ctx, "chunk-2 ", 8);
  xHttpCtxWrite(ctx, "chunk-3", 7);
  xHttpCtxEndStream(ctx);
}

struct EchoCtx {
  std::string body;
};
static int echo_on_data(const char *data, size_t len, void *arg) {
  static_cast<EchoCtx *>(arg)->body.append(data, len);
  return 0;
}
static void echo_on_done(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<EchoCtx *>(arg);
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "application/octet-stream");
  xHttpCtxSend(ctx, c->body.data(), c->body.size());
}

static void status_handler(xHttpCtx *ctx, void * /*arg*/) {
  int code = 0;
  if (ctx->url) {
    const char *slash = strrchr(ctx->url, '/');
    if (slash) code = atoi(slash + 1);
  }
  if (code < 100 || code >= 600) code = 200;
  xHttpCtxSetStatus(ctx, code);
  xHttpCtxSend(ctx, nullptr, 0);
}

/* ── Fixture: starts an HTTP server on a free port ────────────────── */

class HttpClientTest : public ::testing::Test {
protected:
  xpp::EventLoop m_loop;
  xpp::WaitScope m_scope{m_loop};
  xHttpServer    m_server = nullptr;
  xHttpMux       m_mux    = nullptr;
  uint16_t       m_port   = 0;

  void SetUp() override {
    m_port = find_free_port();
    ASSERT_NE(m_port, 0);

    m_mux = xHttpMuxCreate();
    ASSERT_NE(m_mux, nullptr);

    xHttpServerConf conf = {};
    conf.resolve         = xHttpMuxResolve;
    conf.router          = m_mux;
    conf.idle_timeout_ms = 60000;

    m_server = xHttpServerCreate(&conf);
    ASSERT_NE(m_server, nullptr);
    ASSERT_EQ(xHttpServerListen(m_server, "127.0.0.1", m_port), xErrno_Ok);

    for (int i = 0; i < 20; i++)
      xEventLoopRun(m_loop.handle(), X_RUN_ONCE);
  }

  void TearDown() override {
    if (m_server) xHttpServerDestroy(m_server);
    if (m_mux) xHttpMuxDestroy(m_mux);
  }

  void route(const char *pattern, xHttpDoneFunc on_done, void *arg = nullptr) {
    xHttpRouteConf conf = {};
    conf.pattern        = pattern;
    conf.on_done        = on_done;
    conf.arg            = arg;
    ASSERT_EQ(xHttpMuxHandle(m_mux, &conf), xErrno_Ok);
  }

  void route_with_data(const char *pattern, xHttpDataFunc on_data, xHttpDoneFunc on_done,
                       void *arg = nullptr) {
    xHttpRouteConf conf = {};
    conf.pattern        = pattern;
    conf.on_data        = on_data;
    conf.on_done        = on_done;
    conf.arg            = arg;
    ASSERT_EQ(xHttpMuxHandle(m_mux, &conf), xErrno_Ok);
  }

  std::string url(const char *path) const {
    return "http://127.0.0.1:" + std::to_string(m_port) + path;
  }
};

/* ═══════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, CreateClient) {
  auto client = xpp::http::Client::builder().build();
  SUCCEED();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Builder pattern
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, BuilderChaining) {
  auto client = xpp::http::Client::builder().build();
  auto b      = client.get(url("/hello"))
             .header("Accept", "text/html")
             .header("X-Custom", "value")
             .body(xpp::bytes::Bytes::from(std::vector<uint8_t>({1, 2, 3})));
  SUCCEED();
}

/* ═══════════════════════════════════════════════════════════════════
 *  GET request
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, GetHelloWorld) {
  route("/hello", hello_handler);

  auto client = xpp::http::Client::builder().build();
  auto result = client.get(url("/hello")).send().await();

  ASSERT_TRUE(result.is_ok()) << "GET /hello failed";
  auto &&response = result.unwrap();
  EXPECT_EQ(response.status(), 200);

  auto body = response.text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(), "Hello, xpp!");
}

/* ═══════════════════════════════════════════════════════════════════
 *  POST request with body echo
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, PostEcho) {
  EchoCtx echo;
  route_with_data("POST /echo", echo_on_data, echo_on_done, &echo);

  auto        client  = xpp::http::Client::builder().build();
  std::string payload = "echo this back";
  auto        result =
    client.post(url("/echo"))
      .body(xpp::bytes::Bytes::from(std::vector<uint8_t>(payload.begin(), payload.end())))
      .send()
      .await();

  ASSERT_TRUE(result.is_ok());
  auto &&response = result.unwrap();
  EXPECT_EQ(response.status(), 200);

  auto body = response.text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(), payload);
}

/* ═══════════════════════════════════════════════════════════════════
 *  POST with empty body
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, PostEmptyBody) {
  EchoCtx echo;
  route_with_data("POST /echo-empty", echo_on_data, echo_on_done, &echo);

  auto client = xpp::http::Client::builder().build();
  auto result = client.post(url("/echo-empty"))
                  .body(xpp::bytes::Bytes::from(std::vector<uint8_t>()))
                  .send()
                  .await();

  ASSERT_TRUE(result.is_ok());
  auto &&response = result.unwrap();
  EXPECT_EQ(response.status(), 200);

  auto body = response.text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(), "");
}

/* ═══════════════════════════════════════════════════════════════════
 *  POST with binary body
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, PostBinaryBody) {
  EchoCtx echo;
  route_with_data("POST /echo-bin", echo_on_data, echo_on_done, &echo);

  auto                 client  = xpp::http::Client::builder().build();
  std::vector<uint8_t> payload = {0x00, 0x01, 0x02, 0xFE, 0xFF};
  auto                 result  = client.post(url("/echo-bin"))
                  .body(xpp::bytes::Bytes::from(std::vector<uint8_t>(payload)))
                  .send()
                  .await();

  ASSERT_TRUE(result.is_ok());
  EXPECT_EQ(result.unwrap().status(), 200);

  auto body = result.unwrap().bytes().await();
  ASSERT_TRUE(body.is_ok());
  auto &&data = body.unwrap();
  EXPECT_EQ(data.size(), payload.size());
  EXPECT_EQ(std::memcmp(data.data(), payload.data(), payload.size()), 0);
}

/* ═══════════════════════════════════════════════════════════════════
 *  HTTP status code forwarding
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, StatusCode) {
  route("/status/404", status_handler);

  auto client = xpp::http::Client::builder().build();
  auto result = client.get(url("/status/404")).send().await();

  ASSERT_TRUE(result.is_ok());
  EXPECT_EQ(result.unwrap().status(), 404);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Empty body
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, EmptyBody) {
  route("/empty", status_handler);

  auto client = xpp::http::Client::builder().build();
  auto result = client.get(url("/empty")).send().await();

  ASSERT_TRUE(result.is_ok());
  auto &&response = result.unwrap();
  EXPECT_EQ(response.status(), 200);

  auto body = response.text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(), "");
}

/* ═══════════════════════════════════════════════════════════════════
 *  Streaming body access (chunk)
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, ChunkStreaming) {
  route("/chunked", chunked_handler);

  auto client = xpp::http::Client::builder().build();
  auto result = client.get(url("/chunked")).send().await();

  ASSERT_TRUE(result.is_ok());
  auto &&response = result.unwrap();
  EXPECT_EQ(response.status(), 200);

  // Consume body chunk by chunk — verify streaming loop works
  std::string body;
  while (true) {
    auto chunk = response.chunk().await();
    if (chunk.is_none()) break;
    body += chunk.unwrap().to_string();
  }
  EXPECT_EQ(body, "chunk-1 chunk-2 chunk-3");
}
