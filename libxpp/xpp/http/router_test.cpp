/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * router_test.cpp — Standalone Router tests: matching, params, layers,
 * nest/merge, fallback, 404/405. Calls Router::operator() directly —
 * no sockets (the "Router is a handler" unit-test form).
 */

#include <string>

#include <gtest/gtest.h>
#include <xpp/event.h>
#include <xpp/http/router.h>

using namespace xpp;
using namespace xpp::http;

/* ── Helpers ───────────────────────────────────────────────────────── */

static Request make_req(Method::Value m, const char *path) {
  return Request::builder().method(m).url(path).body().unwrap();
}

static Request get(const char *path) {
  return make_req(Method::Get, path);
}

/// Response body as std::string (Once bodies resolve synchronously).
static std::string body_str(xpp::http::Result<Response> &&r) {
  Response resp = std::move(r).unwrap();
  Bytes    b    = resp.into_body().into_once_bytes();
  return std::string(reinterpret_cast<const char *>(b.data()), b.size());
}

static uint16_t status_of(xpp::http::Result<Response> &&r) {
  return r.unwrap().status_code();
}

/// A layer that appends @p tag to the response body (outermost layers
/// append last — tower ordering).
static Router::LayerFn tag_layer(const char *tag) {
  std::string t(tag);
  return [t](Router::HandlerFn next) -> Router::HandlerFn {
    return [t, next](Request req) -> Promise<xpp::http::Result<Response>> {
      return next(std::move(req))
        .then([t](xpp::http::Result<Response> r) -> Promise<xpp::http::Result<Response>> {
          Response    resp = std::move(r).unwrap();
          Bytes       b    = resp.into_body().into_once_bytes();
          std::string s(reinterpret_cast<const char *>(b.data()), b.size());
          s += t;
          return xpp::resolve(http::Result<Response>(xpp::ok, Response::ok(s.c_str())));
        });
    };
  };
}

/* ── Matching & params ─────────────────────────────────────────────── */

TEST(RouterTest, MatchAndTypedParamInjection) {
  EventLoop loop;
  WaitScope scope(loop);

  Router r;
  r.route("GET /users/:id", [](Request req, String id) { return Response::ok(id); });

  auto resp = r(get("/users/42")).await();
  EXPECT_TRUE(resp.is_ok());
  EXPECT_EQ(body_str(std::move(resp)), "42");
}

TEST(RouterTest, AnyMethodPatternMatchesAllMethods) {
  EventLoop loop;
  WaitScope scope(loop);

  Router r;
  r.route("/ping", [](Request) { return Response::ok("pong"); });

  EXPECT_EQ(body_str(r(make_req(Method::Post, "/ping")).await()), "pong");
  EXPECT_EQ(body_str(r(make_req(Method::Delete, "/ping")).await()), "pong");
}

TEST(RouterTest, MethodMismatchAnswers405) {
  EventLoop loop;
  WaitScope scope(loop);

  Router r;
  r.route("GET /a", [](Request) { return Response::ok("a"); });

  EXPECT_EQ(status_of(r(make_req(Method::Post, "/a")).await()), 405);
  // The same route with the right method still works.
  EXPECT_EQ(status_of(r(get("/a")).await()), 200);
}

TEST(RouterTest, UnmatchedPathAnswers404) {
  EventLoop loop;
  WaitScope scope(loop);

  Router r;
  r.route("GET /a", [](Request) { return Response::ok("a"); });

  EXPECT_EQ(status_of(r(get("/nope")).await()), 404);
}

TEST(RouterTest, QueryStringIgnored) {
  EventLoop loop;
  WaitScope scope(loop);

  Router r;
  r.route("GET /users/:id", [](Request, String id) { return Response::ok(id); });

  EXPECT_EQ(body_str(r(get("/users/7?verbose=1&x=2")).await()), "7");
}

TEST(RouterTest, FirstMatchWins) {
  EventLoop loop;
  WaitScope scope(loop);

  Router r;
  r.route("GET /a", [](Request) { return Response::ok("first"); });
  r.route("GET /a", [](Request) { return Response::ok("second"); });

  EXPECT_EQ(body_str(r(get("/a")).await()), "first");
}

TEST(RouterTest, ParamAccessibleViaRequestParam) {
  EventLoop loop;
  WaitScope scope(loop);

  Router r;
  r.route("GET /users/:id/posts/:post",
          [](Request req, String, String) { return Response::ok(req.param("post").unwrap()); });

  EXPECT_EQ(body_str(r(get("/users/1/posts/abc")).await()), "abc");
}

/* ── Fallback ──────────────────────────────────────────────────────── */

TEST(RouterTest, CustomFallbackReplaces404) {
  EventLoop loop;
  WaitScope scope(loop);

  Router r;
  r.route("GET /a", [](Request) { return Response::ok("a"); });
  r.fallback([](Request) { return Response::ok("custom-404"); });

  EXPECT_EQ(body_str(r(get("/nope")).await()), "custom-404");
  // 405 is still automatic with a custom fallback.
  EXPECT_EQ(status_of(r(make_req(Method::Post, "/a")).await()), 405);
}

/* ── Layers (middleware) ───────────────────────────────────────────── */

TEST(RouterTest, LayerOrderFirstRegisteredIsOutermost) {
  EventLoop loop;
  WaitScope scope(loop);

  Router r;
  r.layer(tag_layer("L1"))  // outermost: sees the response last
    .layer(tag_layer("L2")) // inner: appends first on the way out
    .route("GET /h", [](Request) { return Response::ok("H"); });

  EXPECT_EQ(body_str(r(get("/h")).await()), "HL2L1");
}

TEST(RouterTest, LayerShortCircuitSkipsHandler) {
  EventLoop loop;
  WaitScope scope(loop);

  Router r;
  r.layer([](Router::HandlerFn next) -> Router::HandlerFn {
    return [next](Request req) -> Promise<xpp::http::Result<Response>> {
      if (req.headers().get("Authorization").is_none()) {
        return xpp::resolve(http::Result<Response>(
          xpp::ok, ResponseBuilder().status(StatusCode::Unauthorized).body("no auth")));
      }
      return next(std::move(req));
    };
  });
  r.route("GET /secret", [](Request) { return Response::ok("top"); });

  auto rejected = r(get("/secret")).await();
  EXPECT_EQ(rejected.unwrap().status_code(), 401);
  EXPECT_EQ(body_str(std::move(rejected)), "no auth");

  Request authed = Request::builder()
                     .method(Method::Get)
                     .url("/secret")
                     .header("Authorization", "Bearer x")
                     .body()
                     .unwrap();
  EXPECT_EQ(body_str(r(std::move(authed)).await()), "top");
}

TEST(RouterTest, LayerReadsPathParams) {
  EventLoop loop;
  WaitScope scope(loop);

  Router r;
  r.layer([](Router::HandlerFn next) -> Router::HandlerFn {
    return [next](Request req) -> Promise<xpp::http::Result<Response>> {
      if (req.param("id").is_some() && req.param("id").unwrap() == "0") {
        return xpp::resolve(http::Result<Response>(
          xpp::ok, ResponseBuilder().status(StatusCode::Forbidden).body("id 0")));
      }
      return next(std::move(req));
    };
  });
  r.route("GET /users/:id", [](Request, String id) { return Response::ok(id); });

  EXPECT_EQ(status_of(r(get("/users/0")).await()), 403);
  EXPECT_EQ(body_str(r(get("/users/9")).await()), "9");
}

/* ── nest / merge ──────────────────────────────────────────────────── */

TEST(RouterTest, NestStripsPrefixForMatching) {
  EventLoop loop;
  WaitScope scope(loop);

  Router api;
  api.route("/users/:id", [](Request, String id) { return Response::ok(id); });

  Router r;
  r.nest("/api", std::move(api));

  EXPECT_EQ(body_str(r(get("/api/users/7")).await()), "7");
  // The bare path does not match the outer router (only /api/* does).
  EXPECT_EQ(status_of(r(get("/users/7")).await()), 404);
}

TEST(RouterTest, NestLayersOfSubRouterApply) {
  EventLoop loop;
  WaitScope scope(loop);

  Router api;
  api.layer(tag_layer("[api]")).route("/health", [](Request) { return Response::ok("ok"); });

  Router r;
  r.layer(tag_layer("[root]")).nest("/api", std::move(api));

  EXPECT_EQ(body_str(r(get("/api/health")).await()), "ok[api][root]");
}

TEST(RouterTest, MergeCombinesRoutes) {
  EventLoop loop;
  WaitScope scope(loop);

  Router a;
  a.route("GET /a", [](Request) { return Response::ok("A"); });

  Router b;
  b.route("GET /b", [](Request) { return Response::ok("B"); });

  a.merge(std::move(b));

  EXPECT_EQ(body_str(a(get("/a")).await()), "A");
  EXPECT_EQ(body_str(a(get("/b")).await()), "B");
}

TEST(RouterTest, EmptyRouterAnswers404) {
  EventLoop loop;
  WaitScope scope(loop);

  Router r;
  EXPECT_EQ(status_of(r(get("/anything")).await()), 404);
}

/* ── Review regression: depth-2 nest & nested fallback ─────────────── */

TEST(RouterTest, NestLayersApplyAtDepthTwo) {
  EventLoop loop;
  WaitScope scope(loop);

  // top -> mid -> leaf. Expected chain (inner → outer): [leaf][mid][top].
  Router leaf;
  leaf.layer(tag_layer("[leaf]")).route("/h", [](Request) { return Response::ok("H"); });

  Router mid;
  mid.layer(tag_layer("[mid]")).nest("/m", std::move(leaf));

  Router top;
  top.layer(tag_layer("[top]")).nest("/t", std::move(mid));

  EXPECT_EQ(body_str(top(get("/t/m/h")).await()), "H[leaf][mid][top]");
}

TEST(RouterTest, NestedFallbackAnswersUnderItsPrefix) {
  EventLoop loop;
  WaitScope scope(loop);

  Router api;
  api.route("/known", [](Request) { return Response::ok("api"); });
  api.fallback([](Request) { return Response::ok("api-404"); });

  Router r;
  r.route("/root", [](Request) { return Response::ok("root"); });
  r.fallback([](Request) { return Response::ok("root-404"); });
  r.nest("/api", std::move(api));

  // Under /api: the nested fallback answers, not the outer one.
  EXPECT_EQ(body_str(r(get("/api/nope")).await()), "api-404");
  // Outside /api: the outer fallback answers.
  EXPECT_EQ(body_str(r(get("/nope")).await()), "root-404");
  // Nested routes still match normally.
  EXPECT_EQ(body_str(r(get("/api/known")).await()), "api");
  EXPECT_EQ(body_str(r(get("/root")).await()), "root");
}

TEST(RouterTest, NestedFallbackPropagatesFromDepthTwo) {
  EventLoop loop;
  WaitScope scope(loop);

  Router leaf;
  leaf.fallback([](Request) { return Response::ok("leaf-404"); });

  Router mid;
  mid.nest("/m", std::move(leaf));

  Router r;
  r.nest("/t", std::move(mid));

  EXPECT_EQ(body_str(r(get("/t/m/anything")).await()), "leaf-404");
}

TEST(RouterTest, ComposedEndpointOutlivesTheRouter) {
  EventLoop loop;
  WaitScope scope(loop);

  // The Server composes synchronously and spawns only the endpoint
  // (xpp::spawn defers execution). Structural guarantee under test:
  // the composed endpoint is fully self-contained — invoking it after
  // the Router itself is destroyed must not touch freed memory (the
  // pre-fix code dereferenced a raw Router* from the deferred closure).
  Router::HandlerFn endpoint;
  {
    Router r;
    r.layer(tag_layer("[mw]")).route("GET /a", [](Request) { return Response::ok("A"); });
    Request req = get("/a");
    endpoint    = r.compose(req);
    // `r` (and its routes/layers) dies here.
  }

  Request req2 = get("/a");
  auto    resp = endpoint(std::move(req2)).await();
  EXPECT_TRUE(resp.is_ok());
  EXPECT_EQ(body_str(std::move(resp)), "A[mw]");
}
