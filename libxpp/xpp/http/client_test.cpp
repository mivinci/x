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
#include <xpp/bytes/reader.h>
#include <xpp/event.h>
#include <xpp/http/client.h>
#include <xpp/promise.h>

#include <x/http/server.h>

using namespace xpp::http;

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

/* ── Pull-model callbacks (server side) ──────────────────── */

struct EchoCtx {
  std::string body;
  size_t      offset = 0;
};

static int echo_on_data(const char *data, size_t len, void *arg) {
  static_cast<EchoCtx *>(arg)->body.append(data, len);
  return 0;
}
static int echo_on_req(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<EchoCtx *>(arg);
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "application/octet-stream");
  // Don't set Content-Length here — body is not yet available
  // (on_data is called later during body pump).  on_read will
  // signal EOF and the server will use chunked encoding.
  return 0;
}
static size_t echo_on_read(char *buf, size_t bufsize, void *arg) {
  auto *c = static_cast<EchoCtx *>(arg);
  if (c->offset >= c->body.size()) return 0;
  size_t n = std::min(bufsize, c->body.size() - c->offset);
  std::memcpy(buf, c->body.data() + c->offset, n);
  c->offset += n;
  return n;
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

  void route_pull(const char *pattern, const std::string &body, int status = 200,
                  const char *ctype = "text/plain") {
    xHttpRouteConf conf = {};
    conf.pattern        = pattern;
    conf.on_request     = [](xHttpCtx *ctx2, void *arg) -> int {
      auto *p = static_cast<std::tuple<int, std::string, std::string> *>(arg);
      xHttpCtxSetStatus(ctx2, std::get<0>(*p));
      auto &ct = std::get<2>(*p);
      if (!ct.empty()) xHttpCtxSetHeader(ctx2, "Content-Type", ct.c_str());
      auto &bd = std::get<1>(*p);
      if (!bd.empty()) xHttpCtxSetHeader(ctx2, "Content-Length", std::to_string(bd.size()).c_str());
      return 0;
    };
    conf.on_read = [](char *buf, size_t, void *arg) -> size_t {
      auto         *p   = static_cast<std::tuple<int, std::string, std::string> *>(arg);
      static size_t off = 0;
      auto         &b   = std::get<1>(*p);
      if (off >= b.size()) {
        off = 0;
        return 0;
      }
      size_t n = std::min(static_cast<size_t>(4096), b.size() - off);
      std::memcpy(buf, b.data() + off, n);
      off += n;
      return n;
    };
    static std::tuple<int, std::string, std::string> store;
    store    = {status, body, ctype ? std::string(ctype) : std::string()};
    conf.arg = &store;
    ASSERT_EQ(xHttpMuxHandle(m_mux, &conf), xErrno_Ok);
  }

  void route_pull_echo(const char *pattern, EchoCtx *echo) {
    xHttpRouteConf conf = {};
    conf.pattern        = pattern;
    conf.on_request     = echo_on_req;
    conf.on_read        = echo_on_read;
    conf.on_data        = echo_on_data;
    conf.arg            = echo;
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
 *  GET request
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, GetHelloWorld) {
  route_pull("/hello", "Hello, xpp!");

  auto client = xpp::http::Client::builder().build();
  auto req    = Request::builder().method(Method::Get).url(url("/hello")).body().unwrap();
  auto result = client.send(std::move(req)).await();

  ASSERT_TRUE(result.is_ok()) << "GET /hello failed";
  auto &&response = result.unwrap();
  EXPECT_EQ(response.status(), 200);

  auto body = response.text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(), "Hello, xpp!");
}

/* ═══════════════════════════════════════════════════════════════════
 *  POST request with body echo (TryRead body)
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, PostEcho) {
  EchoCtx echo;
  route_pull_echo("POST /echo", &echo);

  auto        client  = xpp::http::Client::builder().build();
  std::string payload = "echo this back";
  auto        req     = Request::builder()
               .method(Method::Post)
               .url(url("/echo"))
               .header("Content-Length", std::to_string(payload.size()))
               .body(xpp::bytes::Reader(xpp::bytes::Bytes::copy(
                 reinterpret_cast<const uint8_t *>(payload.data()), payload.size())))
               .unwrap();
  auto result = client.send(std::move(req)).await();

  ASSERT_TRUE(result.is_ok());
  auto &&response = result.unwrap();
  EXPECT_EQ(response.status(), 200);

  auto body = response.text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(), payload);
}

/* ═══════════════════════════════════════════════════════════════════
 *  HTTP status code forwarding
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, StatusCode) {
  {
    xHttpRouteConf rc = {};
    rc.pattern        = "/status/404";
    rc.on_request     = [](xHttpCtx *ctx, void *) -> int {
      int code = 200;
      if (ctx->url) {
        const char *slash = strrchr(ctx->url, '/');
        if (slash) code = atoi(slash + 1);
      }
      if (code < 100 || code >= 600) code = 200;
      xHttpCtxSetStatus(ctx, code);
      return 0;
    };
    rc.on_read = [](char *, size_t, void *) -> size_t { return 0; };
    ASSERT_EQ(xHttpMuxHandle(m_mux, &rc), xErrno_Ok);
  }

  auto client = xpp::http::Client::builder().build();
  auto req    = Request::builder().method(Method::Get).url(url("/status/404")).body().unwrap();
  auto result = client.send(std::move(req)).await();

  ASSERT_TRUE(result.is_ok());
  EXPECT_EQ(result.unwrap().status(), 404);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Empty body response
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, EmptyBody) {
  {
    xHttpRouteConf rc = {};
    rc.pattern        = "/empty";
    rc.on_request     = [](xHttpCtx *ctx, void *) -> int {
      xHttpCtxSetStatus(ctx, 200);
      return 0;
    };
    rc.on_read = [](char *, size_t, void *) -> size_t { return 0; };
    ASSERT_EQ(xHttpMuxHandle(m_mux, &rc), xErrno_Ok);
  }

  auto client = xpp::http::Client::builder().build();
  auto req    = Request::builder().method(Method::Get).url(url("/empty")).body().unwrap();
  auto result = client.send(std::move(req)).await();

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

TEST_F(HttpClientTest, StreamingBodyViaText) {
  {
    xHttpRouteConf rc = {};
    rc.pattern        = "/chunked";
    rc.on_request     = [](xHttpCtx *ctx, void *) -> int {
      xHttpCtxSetStatus(ctx, 200);
      xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
      return 0;
    };
    rc.on_read = [](char *buf, size_t, void *) -> size_t {
      static const char *chunks = "chunk-1 chunk-2 chunk-3";
      static size_t      off    = 0;
      if (off >= 23) {
        off = 0;
        return 0;
      }
      buf[0] = chunks[off];
      off++;
      return 1;
    };
    ASSERT_EQ(xHttpMuxHandle(m_mux, &rc), xErrno_Ok);
  }

  auto client = xpp::http::Client::builder().build();
  auto req    = Request::builder().method(Method::Get).url(url("/chunked")).body().unwrap();
  auto result = client.send(std::move(req)).await();

  ASSERT_TRUE(result.is_ok());
  auto &&response = result.unwrap();
  EXPECT_EQ(response.status(), 200);

  auto body = response.text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(), "chunk-1 chunk-2 chunk-3");
}

/* ═══════════════════════════════════════════════════════════════════
 *  POST with binary body echo
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, PostBinaryBody) {
  EchoCtx echo;
  route_pull_echo("POST /echo-bin", &echo);

  auto                 client  = xpp::http::Client::builder().build();
  std::vector<uint8_t> payload = {0x00, 0x01, 0x02, 0xFE, 0xFF};
  auto                 req     = Request::builder()
               .method(Method::Post)
               .url(url("/echo-bin"))
               .header("Content-Length", std::to_string(payload.size()))
               .body(xpp::bytes::Reader(xpp::bytes::Bytes::from(std::vector<uint8_t>(payload))))
               .unwrap();
  auto result = client.send(std::move(req)).await();

  ASSERT_TRUE(result.is_ok());
  EXPECT_EQ(result.unwrap().status(), 200);

  auto body = result.unwrap().text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(),
            std::string(reinterpret_cast<const char *>(payload.data()), payload.size()));
}
