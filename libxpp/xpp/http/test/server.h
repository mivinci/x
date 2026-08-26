/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server.h — xpp::http::test::Server: real-TCP HTTP test fixture.
 *
 * Layer 1 of the three-layer HTTP testing model (Rust-ecosystem aligned,
 * see todos/httptest.md):
 *
 *   Layer 0  router(req) direct call        — handler/router/middleware
 *            (in-process, no sockets)         unit tests (Router is a Handler)
 *   Layer 1  THIS FILE — test::Server       — client tests & end-to-end:
 *            (real TCP, ephemeral port)        the thing under test needs
 *                                             a real endpoint
 *   Layer 2  test::EvilServer (evil_server.h)— fault injection (a
 *                                             well-formed server can't lie
 *                                             about framing)
 *
 * Construction starts the server (bind 127.0.0.1:0 + listen synchronously
 * — port() is valid immediately); RAII destructor stops and drains. Bind
 * failure aborts: the environment is broken and the test cannot proceed
 * (mirrors Go httptest.NewServer panicking on listen failure).
 *
 * Test-only. NOT part of any public umbrella — include explicitly:
 *   #include <xpp/http/test/server.h>
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_HTTP_TEST_SERVER_H
#define XPP_HTTP_TEST_SERVER_H

#include <cstdint>
#include <string>
#include <utility>

#include <xpp/bytes.h>
#include <xpp/http/request.h>
#include <xpp/http/response.h>
#include <xpp/http/router.h>
#include <xpp/http/server.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/spawn.h>
#include <xpp/string.h>
#include <xpp/sync/mpsc.h>
#include <xpp/vec.h>

namespace xpp {
namespace http {
namespace test {

/* ── First-arg trait (constructor dispatch) — must precede Server ── */

namespace _ {
template <class T> struct first_arg;
template <class Ret, class A0> struct first_arg<Ret(A0)> {
  using type = A0;
};
template <class Ret, class A0> struct first_arg<Ret (*)(A0)> {
  using type = A0;
};
template <class Ret, class C, class A0> struct first_arg<Ret (C::*)(A0) const> {
  using type = A0;
};
template <class T> struct first_arg : first_arg<decltype(&T::operator())> {};
} // namespace _

/**
 * @brief Preset HTTP response for the data-driven constructor.
 *
 * The spec compiles into Router routes at construction — no hand-rolled
 * HTTP anywhere (the Router + Server own parsing and framing).
 */
struct TestResponseSpec {
  StatusCode::Value              status = StatusCode::Ok;
  Vec<std::pair<String, String>> headers;
  Bytes                          body;
  /** Pre-response delay in ms (client-timeout tests). 0 = immediate. */
  uint64_t delay_ms = 0;
  /** Echo the request body back (verifies POST payload transmission). */
  bool echo_request_body = false;
  /** Add `X-Echo-Method: <method>` (verifies the client sent the verb). */
  bool echo_request_method = false;
  /**
   * Redirect requests whose path is not @p redirect_to to it with
   * 302 + Location (client redirect-following tests). The final request
   * is served with the rest of this spec.
   */
  String redirect_to;
  /**
   * Stall this many ms after half the body is sent, then continue
   * (read-timeout / slow-peer tests). Implemented as a channel body with
   * a delayed producer: pausing production pauses transmission — the
   * server only writes what has been produced.
   */
  uint64_t mid_body_delay_ms = 0;
};

/**
 * @brief Real-TCP HTTP test server (client tests & end-to-end).
 *
 * @code
 *   // Handler-driven (any behavior):
 *   test::Server ts([](Request req) { return Response::ok("hello"); });
 *   auto r = client.get(ts.url().c_str()).await();
 *
 *   // Multi-route (Router capabilities: :params, layers, async handlers):
 *   test::Server routed([](ServerBuilder &b) {
 *     b.route("GET /users/:id", [](Request req, String id) {
 *       return Response::ok(id);
 *     });
 *   });
 *
 *   // Data-driven (preset spec):
 *   TestResponseSpec spec;
 *   spec.body = Bytes::from("hello");
 *   test::Server ts2(spec);
 * @endcode
 */
class Server {
public:
  Server()                              = default;
  Server(Server &&) noexcept            = default;
  Server &operator=(Server &&) noexcept = default;
  Server(const Server &)                = delete;
  Server &operator=(const Server &)     = delete;

  ~Server() {
    close();
  }

  /**
   * @brief Builder-configurator constructor (multi-route).
   *
   * @p configure receives the ServerBuilder — register routes, layers,
   * set timeouts as usual. The server binds 127.0.0.1:0 and listens
   * synchronously before the constructor returns.
   */
  /// Data-driven: the preset spec compiles into Router routes.
  explicit Server(TestResponseSpec spec) {
    init_dispatch([&spec](ServerBuilder &b) { build_spec_routes(b, spec); }, std::true_type());
  }

  /**
   * @brief Generic constructor — dispatches on the callable's parameter.
   *
   * - `[](ServerBuilder &b) { ... }` → builder-configurator (multi-route,
   *   layers, timeouts — the general form)
   * - `[](Request req) { return ...; }` → single handler (registered as
   *   the Router fallback, receives every request)
   */
  template <class F, class = typename std::enable_if<
                       !std::is_same<typename std::decay<F>::type, TestResponseSpec>::value>::type>
  explicit Server(F &&f) {
    // Dispatch: does F take (ServerBuilder&) or (Request)?
    using Arg0 = typename _::first_arg<typename std::decay<F>::type>::type;
    init_dispatch(std::forward<F>(f),
                  std::is_same<typename std::decay<Arg0>::type, ServerBuilder>());
  }

  /** @brief "http://127.0.0.1:<port>" — ready to hand to a client. */
  String url() const {
    std::string u = "http://127.0.0.1:" + std::to_string(port());
    return String::from_utf8(u.c_str()).unwrap();
  }

  /** @brief The kernel-assigned port. */
  uint16_t port() const noexcept {
    return m_server.is_some() ? m_server.unwrap().port() : 0;
  }

  /** @brief Stop the server and drain. Idempotent. */
  void close() {
    if (m_server.is_some()) {
      m_server.unwrap().stop();
      if (m_running.is_some()) {
        m_running.unwrap().await();
        m_running = none;
      }
      m_server = none;
    }
  }

private:
  template <class F> void init_dispatch(F &&configure, std::true_type) {
    // Builder-configurator: (ServerBuilder&) → void
    ServerBuilder builder;
    configure(builder);
    m_server  = some(builder.bind("127.0.0.1", 0).build().unwrap());
    m_running = some(m_server.unwrap().serve());
  }

  template <class H> void init_dispatch(H &&handler, std::false_type) {
    // Single handler: (Request) → Response-ish — Router fallback
    ServerBuilder builder;
    Router        r;
    r.fallback(std::forward<H>(handler));
    builder.router(std::move(r));
    m_server  = some(builder.bind("127.0.0.1", 0).build().unwrap());
    m_running = some(m_server.unwrap().serve());
  }

  static void build_spec_routes(ServerBuilder &b, const TestResponseSpec &spec);

  Option<http::Server>          m_server;
  Option<Promise<Result<void>>> m_running;
};

/* ── Spec → Router compilation ─────────────────────────────────────── */

namespace _ {

/// Request path from the URL (strip scheme/host if present, strip query).
inline String request_path(const Request &req) {
  auto   ub  = req.url().as_bytes();
  size_t len = ub.size();
  // Strip query
  for (size_t i = 0; i < len; ++i) {
    if (ub.data()[i] == '?') {
      len = i;
      break;
    }
  }
  return String::from_utf8(reinterpret_cast<const char *>(ub.data()), len).unwrap();
}

/// Build the response for the spec (sync part — no request-body reads).
inline Response build_spec_response(const TestResponseSpec &spec, const Request &req) {
  ResponseBuilder rb;
  rb.status(spec.status);
  for (auto &h : spec.headers) {
    rb.header(h.first, h.second);
  }
  if (spec.echo_request_method) {
    rb.header(String::from_utf8("X-Echo-Method").unwrap(),
              String::from_utf8(to_string(req.method())).unwrap());
  }
  return rb.body(spec.body);
}

/// Build a 302 redirect to the target.
inline Response build_redirect(const String &to) {
  return ResponseBuilder()
    .status(StatusCode::Found)
    .header(String::from_utf8("Location").unwrap(), to)
    .body();
}

} // namespace _

/* ── build_spec_routes ─────────────────────────────────────────────── */

inline void Server::build_spec_routes(ServerBuilder &b, const TestResponseSpec &spec) {
  // The spec dispatch runs as the Router fallback (every request hits it).
  Router r;
  r.fallback([spec](Request req) -> Promise<Result<Response>> {
    // 1. Redirect: any path != redirect_to → 302 + Location.
    if (!spec.redirect_to.empty()) {
      String path = _::request_path(req);
      if (path != spec.redirect_to) {
        return xpp::resolve(Result<Response>(xpp::ok, _::build_redirect(spec.redirect_to)));
      }
    }

    // 2. Echo request body: drain the body, return it.
    if (spec.echo_request_body) {
      // Capture the method BEFORE the request is consumed by into_body().
      String method = String::from_utf8(to_string(req.method())).unwrap();
      return req.into_body().bytes().then(
        [spec, method](Result<Bytes> b) -> Promise<Result<Response>> {
          ResponseBuilder rb;
          rb.status(spec.status);
          for (auto &h : spec.headers) {
            rb.header(h.first, h.second);
          }
          if (spec.echo_request_method) {
            rb.header(String::from_utf8("X-Echo-Method").unwrap(), method);
          }
          return xpp::resolve(Result<Response>(xpp::ok, rb.body(std::move(b).unwrap())));
        });
    }

    // 3. Mid-body stall: channel body with a delayed producer.
    if (spec.mid_body_delay_ms > 0 && spec.body.size() > 0) {
      auto pair = sync::mpsc::channel<Bytes>(4);
      auto tx   = std::move(pair.first);
      auto rx   = std::move(pair.second);

      Bytes body = spec.body;
      // First half now, second half after the stall.
      size_t half  = body.size() / 2;
      Bytes  first = Bytes::copy(reinterpret_cast<const char *>(body.data()), half);
      Bytes  second =
        Bytes::copy(reinterpret_cast<const char *>(body.data() + half), body.size() - half);

      // Sender is move-only with non-const send/close — heap-hold it for
      // the .then chain (C++11 has no move-capture).
      auto     tx_holder = Arc<sync::mpsc::Sender<Bytes>>::make(std::move(tx));
      uint64_t ms        = spec.mid_body_delay_ms;
      xpp::spawn([tx_holder, first, second, ms]() -> Promise<void> {
        return tx_holder->send(first).then([tx_holder, second, ms]() -> Promise<void> {
          return xpp::after(ms).then([tx_holder, second]() -> Promise<void> {
            return tx_holder->send(second).then([tx_holder]() -> Promise<void> {
              tx_holder->close();
              return xpp::resolve();
            });
          });
        });
      });

      ResponseBuilder rb;
      rb.status(spec.status);
      for (auto &h : spec.headers) {
        rb.header(h.first, h.second);
      }
      if (spec.echo_request_method) {
        rb.header(String::from_utf8("X-Echo-Method").unwrap(),
                  String::from_utf8(to_string(req.method())).unwrap());
      }
      return xpp::resolve(Result<Response>(xpp::ok, rb.body(Body::from_channel(std::move(rx)))));
    }

    // 4. Delay then respond.
    if (spec.delay_ms > 0) {
      TestResponseSpec s      = spec;
      String           method = String::from_utf8(to_string(req.method())).unwrap();
      return xpp::after(s.delay_ms).then([s, method]() -> Promise<Result<Response>> {
        ResponseBuilder rb;
        rb.status(s.status);
        for (auto &h : s.headers) {
          rb.header(h.first, h.second);
        }
        if (s.echo_request_method) {
          rb.header(String::from_utf8("X-Echo-Method").unwrap(), method);
        }
        return xpp::resolve(Result<Response>(xpp::ok, rb.body(s.body)));
      });
    }

    // 5. Immediate response.
    return xpp::resolve(Result<Response>(xpp::ok, _::build_spec_response(spec, req)));
  });
  b.router(std::move(r));
}

} // namespace test
} // namespace http
} // namespace xpp

#endif // XPP_HTTP_TEST_SERVER_H
