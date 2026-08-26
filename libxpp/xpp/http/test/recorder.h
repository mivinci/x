/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * recorder.h — xpp::http::test::Recorder: request capture + assertions.
 *
 * The wiremock `.expect(n)` + `.verify()` equivalent — but synchronous
 * (single-threaded event loop, no async verify machinery). Wrap any
 * handler; every incoming request is recorded before the handler runs.
 * The test asserts afterwards.
 *
 * @code
 *   test::Recorder rec([](Request) { return Response::ok("ok"); });
 *   test::Server ts(rec.handler());
 *
 *   client.get(url_of(ts, "/a").c_str()).await();
 *   client.post(url_of(ts, "/b").c_str(), "x").await();
 *
 *   EXPECT_EQ(rec.count(), 2);
 *   EXPECT_EQ(rec.at(0).unwrap().method, "GET");
 *   EXPECT_EQ(rec.at(1).unwrap().path, "/b");
 * @endcode
 *
 * Test-only. NOT part of any public umbrella.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_HTTP_TEST_RECORDER_H
#define XPP_HTTP_TEST_RECORDER_H

#include <functional>

#include <xpp/http/request.h>
#include <xpp/http/response.h>
#include <xpp/http/router.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/string.h>
#include <xpp/vec.h>

namespace xpp {
namespace http {
namespace test {

/**
 * @brief One recorded incoming request.
 */
struct RecordedRequest {
  String method; ///< "GET", "POST", ...
  String path;   ///< "/users/42" (query stripped)
};

/**
 * @brief Request recorder — wiremock's expect/verify, synchronously.
 *
 * Wraps a handler; every request is recorded (method + path) before
 * the handler runs. The test asserts via count()/at() after driving
 * the client. Single-threaded loop — no synchronization.
 *
 * The Recorder must outlive the test::Server (the handler lambda
 * captures `this`); declare it before the Server and both locals'
 * destruction order handles it naturally.
 */
class Recorder {
public:
  Recorder() = default;

  /**
   * @brief Wrap a handler — recorded requests delegate to it.
   *
   * @p handler: (Request) → Response / Result<Response> /
   * Promise<Result<Response>> (the standard handler forms).
   */
  template <class H> explicit Recorder(H &&handler) {
    using H_t = typename std::decay<H>::type;
    m_handler = Router::HandlerFn([handler](Request req) -> Promise<Result<Response>> {
      return _::adapt_handler_result(handler(std::move(req)));
    });
  }

  /**
   * @brief The handler to hand to test::Server (or Router::fallback).
   *
   * Captures `this` — the Recorder must outlive the Server.
   */
  Router::HandlerFn handler() {
    return [this](Request req) -> Promise<Result<Response>> {
      RecordedRequest r;
      r.method = String::from_utf8(to_string(req.method())).unwrap();
      // Path: strip query
      auto   ub  = req.url().as_bytes();
      size_t len = ub.size();
      for (size_t i = 0; i < len; ++i) {
        if (ub.data()[i] == '?') {
          len = i;
          break;
        }
      }
      r.path = String::from_utf8(reinterpret_cast<const char *>(ub.data()), len).unwrap();
      m_recorded.push(std::move(r));
      return m_handler(std::move(req));
    };
  }

  /** @brief Number of requests recorded. */
  size_t count() const {
    return m_recorded.len();
  }

  /** @brief The i-th recorded request (None if out of range). */
  Option<RecordedRequest> at(size_t i) const {
    if (i >= m_recorded.len()) return none;
    return xpp::some(m_recorded[i]);
  }

  void clear() {
    m_recorded.clear();
  }

private:
  std::function<Promise<Result<Response>>(Request)> m_handler;
  Vec<RecordedRequest>                              m_recorded;
};

} // namespace test
} // namespace http
} // namespace xpp

#endif // XPP_HTTP_TEST_RECORDER_H
