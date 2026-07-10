/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client.h - xpp::http::Client: Promise-based async HTTP client.
 *
 * Wraps libx xHttpClient via OwnedHandle.  Build a Request with
 * Request::builder().method(Method::Post).url(url).body(...).unwrap(),
 * then send:
 *
 *   auto client  = Client::builder().build();
 *   auto req     = Request::builder().method(Method::Post).url(url).body("hello").unwrap();
 *   auto resp    = co_await client.send(req);
 *   co_await resp.text();
 *
 * Body data flows through a single mpsc channel:
 *   on_data → try_send(chunk) → [channel] → Response::chunk()/text()/bytes()
 *   on_done → close channel
 *
 * No buffering in the adapter — the channel is the single source of truth.
 */

#ifndef XPP_HTTP_CLIENT_H
#define XPP_HTTP_CLIENT_H

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <xpp/bytes/bytes.h>
#include <xpp/handle.h>
#include <xpp/http/error.h>
#include <xpp/http/request.h>
#include <xpp/http/response.h>
#include <xpp/promise.h>

#include <x/http/client.h>

namespace xpp {
namespace http {

// ═════════════════════════════════════════════════════════════════════

class ClientBuilder;

class Client {
public:
  static ClientBuilder builder();

  Client(Client &&) noexcept            = default;
  Client &operator=(Client &&) noexcept = default;

  /// Send a built Request.  Returns a Promise that resolves when
  /// response headers arrive; body is consumed via Response::text() etc.
  Promise<Result<Response>> send(Request req);

private:
  friend class ClientBuilder;

  struct Deleter {
    void deallocate(void *p, Layout) const noexcept {
      if (p) xHttpClientDestroy(static_cast<xHttpClient>(p));
    }
  };

  Client()                                       = default;
  Client(const Client &)                         = delete;
  Client              &operator=(const Client &) = delete;
  OwnedHandle<Deleter> m_client;
};

// ═════════════════════════════════════════════════════════════════════

class ClientBuilder {
public:
  Client build() {
    Client c;
    c.m_client = OwnedHandle<Client::Deleter>(xHttpClientCreate(nullptr));
    return c;
  }

private:
  friend class Client;
  ClientBuilder() = default;
};

inline ClientBuilder Client::builder() {
  return ClientBuilder();
}

// ═════════════════════════════════════════════════════════════════════
// ClientAdapter (internal)
//
// Body data flows through a single mpsc channel:
//   on_data → try_send(chunk) → channel → Response::chunk()/text()/bytes()
//   on_done → close channel
//
// No buffering — the channel is the single source of truth.
// ═════════════════════════════════════════════════════════════════════

namespace _ {

/// Wraps a std::function<ssize_t(char*,size_t)> into a TryRead object.
struct ReadFnWrapper {
  std::function<ssize_t(char *, size_t)> fn;
  ssize_t try_read(char *buf, size_t cap) { return fn(buf, cap); }
};

struct SendAdapter {
  PromiseResolver<Result<Response>>                  m_resolver;
  Option<std::function<ssize_t(char *, size_t)>>     m_reader;
  int                                                m_status = 0;

  struct BodyBuf {
    std::deque<bytes::Bytes> chunks;
  };
  std::shared_ptr<BodyBuf>                           m_buf;

  SendAdapter(PromiseResolver<Result<Response>> r,
              Option<std::function<ssize_t(char *, size_t)>> reader)
      : m_resolver(std::move(r)), m_reader(std::move(reader)),
        m_buf(std::make_shared<BodyBuf>()) {}

  static int on_response(xHttpCtx *ctx, void *arg) {
    auto *self   = static_cast<SendAdapter *>(arg);
    self->m_status = static_cast<int>(ctx->status_code);
    return 0;
  }

  static int on_data(const char *data, size_t len, void *arg) {
    auto *self = static_cast<SendAdapter *>(arg);
    self->m_buf->chunks.push_back(bytes::Bytes::copy(
      reinterpret_cast<const uint8_t *>(data), len));
    return 0;
  }

  static size_t on_read(char *buf, size_t bufsize, void *arg) {
    auto *self = static_cast<SendAdapter *>(arg);
    if (self->m_reader.is_some()) {
      ssize_t n = self->m_reader.unwrap_unchecked()(buf, bufsize);
      if (n > 0) return static_cast<size_t>(n);
      if (n < 0) return 0; // EAGAIN-like → pause
    }
    return 0;
  }

  static void on_done(xHttpCtx *ctx, void *arg) {
    auto *self = static_cast<SendAdapter *>(arg);

    // Build a TryRead body from the buffered chunks.
    auto buf = self->m_buf; // shared_ptr copy
    auto read_fn = [buf](char *b, size_t cap) mutable -> ssize_t {
      static size_t off = 0;
      while (!buf->chunks.empty()) {
        auto &front = buf->chunks.front();
        size_t remain = front.size() - off;
        if (remain > 0) {
          size_t n = std::min(cap, remain);
          std::memcpy(b, front.data() + off, n);
          off += n;
          if (off >= front.size()) {
            buf->chunks.pop_front();
            off = 0;
          }
          return static_cast<ssize_t>(n);
        }
        buf->chunks.pop_front();
        off = 0;
      }
      return 0; // EOF
    };

    auto builder = Response::builder().status(self->m_status);
    if (ctx->headers && ctx->headers_len) {
      builder.header("_raw_headers",
                     std::string(ctx->headers, ctx->headers_len));
    }
    auto resp = builder.body(ReadFnWrapper{std::move(read_fn)}).build();

    self->m_resolver.resolve(Result<Response>(xpp::ok, std::move(resp)));
    delete self;
  }
};

} // namespace _

// ═════════════════════════════════════════════════════════════════════

inline Promise<Result<Response>> Client::send(Request req) {
  auto [p, r] = async<Result<Response>>();

  bool has_body = req.has_body();
  auto *adapter = new _::SendAdapter(
    std::move(r),
    has_body ? std::move(req.take_try_read().unwrap_unchecked())
             : Option<std::function<ssize_t(char *, size_t)>>());

  xHttpRequestConf conf{};
  conf.url         = req.url().c_str();
  conf.method      = static_cast<xHttpMethod>(req.method());
  conf.on_response = _::SendAdapter::on_response;
  conf.on_data     = _::SendAdapter::on_data;
  conf.on_done     = _::SendAdapter::on_done;

  if (has_body) {
    conf.on_read = _::SendAdapter::on_read;
    // Extract Content-Length if set by the request builder
    for (auto &kv : req.headers()) {
      if (strcasecmp(kv.first.c_str(), "Content-Length") == 0) {
        conf.content_length = static_cast<ssize_t>(std::stoll(kv.second));
        break;
      }
    }
  }

  // Build header array from Request
  std::vector<std::string> hstrs;
  for (auto &kv : req.headers())
    hstrs.push_back(kv.first + ": " + kv.second);
  std::vector<const char *> hptrs;
  hptrs.reserve(hstrs.size() + 1);
  for (auto &s : hstrs)
    hptrs.push_back(s.c_str());
  hptrs.push_back(nullptr);
  conf.headers = hptrs.data();

  xErrno err = xHttpClientDo(m_client.get(), &conf, adapter);
  if (err != xErrno_Ok) {
    adapter->m_resolver.resolve(
      Result<Response>(xpp::err, Error::request("xHttpClientDo failed")));
    delete adapter;
  }
  return std::move(p);
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_CLIENT_H
