/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client.h - xpp::http::Client: Promise-based async HTTP client.
 *
 * Wraps libx xHttpClient via OwnedHandle. Request pipeline via
 * HttpAdapter (heap, self-delete). Response resolved at header time,
 * body arrives asynchronously via text()/bytes().
 *
 * Aligned with reqwest API.
 */

#ifndef XPP_HTTP_CLIENT_H
#define XPP_HTTP_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <xpp/bytes/bytes_mut.h>
#include <xpp/handle.h>
#include <xpp/http/error.h>
#include <xpp/http/response.h>
#include <xpp/promise.h>

#include <x/http/client.h>

namespace xpp {
namespace http {

enum class Method {
  Get,
  Post,
  Put,
  Delete,
  Patch,
  Head
};

// ═════════════════════════════════════════════════════════════════════

class RequestBuilder {
public:
  RequestBuilder &header(const std::string &key, const std::string &value) {
    m_headers.emplace_back(key + ": " + value);
    return *this;
  }

  RequestBuilder &body(std::vector<uint8_t> data) {
    m_body = std::move(data);
    return *this;
  }

  Promise<Result<Response>> send();

private:
  friend class Client;
  xHttpClient              m_client;
  Method                   m_method;
  std::string              m_url;
  std::vector<std::string> m_headers;
  std::vector<uint8_t>     m_body;

  RequestBuilder(xHttpClient c, Method m, std::string url)
      : m_client(c), m_method(m), m_url(std::move(url)) {}
};

// ═════════════════════════════════════════════════════════════════════

class Client {
public:
  static Client create() {
    Client c;
    c.m_client = OwnedHandle<Deleter>(xHttpClientCreate(nullptr));
    return c;
  }

  RequestBuilder get(std::string url) {
    return RequestBuilder(m_client.get(), Method::Get, std::move(url));
  }

  RequestBuilder post(std::string url) {
    return RequestBuilder(m_client.get(), Method::Post, std::move(url));
  }

private:
  struct Deleter {
    void deallocate(void *p, Layout) const noexcept {
      if (p) xHttpClientDestroy(static_cast<xHttpClient>(p));
    }
  };
  OwnedHandle<Deleter> m_client;
};

// ═════════════════════════════════════════════════════════════════════
// HttpAdapter (internal)
// ═════════════════════════════════════════════════════════════════════

namespace _ {

struct HttpAdapter {
  PromiseResolver<Result<Response>>     m_resolver;
  PromiseResolver<Result<bytes::Bytes>> m_body_resolver;
  bytes::BytesMut                       m_body_buf;
  Response                              m_response;

  HttpAdapter(PromiseResolver<Result<Response>> r, PromiseResolver<Result<bytes::Bytes>> br)
      : m_resolver(std::move(r)), m_body_resolver(std::move(br)) {}

  static int on_response(xHttpCtx *ctx, void *arg) {
    auto *self = static_cast<HttpAdapter *>(arg);
    self->m_response.set_status(static_cast<int>(ctx->status_code));
    self->m_resolver.resolve(Result<Response>(xpp::ok, std::move(self->m_response)));
    return 0;
  }

  static int on_data(const char *data, size_t len, void *arg) {
    auto *self = static_cast<HttpAdapter *>(arg);
    self->m_body_buf.put(reinterpret_cast<const uint8_t *>(data), len);
    return 0;
  }

  static void on_done(xHttpCtx *ctx, void *arg) {
    auto *self = static_cast<HttpAdapter *>(arg);
    self->m_body_resolver.resolve(Result<bytes::Bytes>(xpp::ok, self->m_body_buf.freeze()));
    delete self;
  }
};

} // namespace _

// ═════════════════════════════════════════════════════════════════════

inline Promise<Result<Response>> RequestBuilder::send() {
  auto [body_p, body_r] = async<Result<bytes::Bytes>>();
  auto [p, r]           = async<Result<Response>>();
  auto *adapter         = new _::HttpAdapter(std::move(r), std::move(body_r));

  adapter->m_response.set_body(std::move(body_p));

  xHttpRequestConf conf{};
  conf.url         = m_url.c_str();
  conf.method      = static_cast<xHttpMethod>(m_method);
  conf.on_response = _::HttpAdapter::on_response;
  conf.on_data     = _::HttpAdapter::on_data;
  conf.on_done     = _::HttpAdapter::on_done;

  std::vector<const char *> hptrs;
  for (auto &s : m_headers)
    hptrs.push_back(s.c_str());
  hptrs.push_back(nullptr);
  conf.headers = hptrs.empty() ? nullptr : hptrs.data();

  xHttpClientDo(m_client, &conf, adapter);
  return std::move(p);
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_CLIENT_H
