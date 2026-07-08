/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client.h - xpp::http::Client: Promise-based async HTTP client.
 *
 * Wraps libx xHttpClient via OwnedHandle. Request pipeline via
 * ClientAdapter (heap, self-delete). Response resolved at header time,
 * body arrives asynchronously via text()/bytes().
 *
 * The entry point:
 *
 *   auto client = Client::builder().build();
 *   auto resp = client.get(url).send().await().unwrap();
 *   auto text = resp.text().await().unwrap();
 *
 * For bodies: .body(bytes::Bytes) for static data.
 *
 * Aligned with reqwest API.
 */

#ifndef XPP_HTTP_CLIENT_H
#define XPP_HTTP_CLIENT_H

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <xpp/bytes/bytes.h>
#include <xpp/bytes/bytes_mut.h>
#include <xpp/bytes/reader.h>
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
    m_headers.emplace(key, value);
    return *this;
  }

  /// Set request body from bytes.
  RequestBuilder &body(bytes::Bytes data) {
    m_body     = std::move(data);
    m_has_body = true;
    return *this;
  }

  Promise<Result<Response>> send();

private:
  friend class Client;
  xHttpClient                             m_client;
  Method                                  m_method;
  std::string                             m_url;
  std::multimap<std::string, std::string> m_headers;
  bytes::Bytes                            m_body;
  bool                                    m_has_body = false;

  RequestBuilder(xHttpClient c, Method m, std::string url)
      : m_client(c), m_method(m), m_url(std::move(url)) {}
};

// ═════════════════════════════════════════════════════════════════════

class ClientBuilder;

class Client {
public:
  static ClientBuilder builder();

  Client(Client &&) noexcept            = default;
  Client &operator=(Client &&) noexcept = default;

  RequestBuilder get(std::string url) {
    return RequestBuilder(m_client.get(), Method::Get, std::move(url));
  }

  RequestBuilder post(std::string url) {
    return RequestBuilder(m_client.get(), Method::Post, std::move(url));
  }

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
// @tparam R  TryRead type with ssize_t try_read(char*, size_t).
//            bytes::Reader is the built-in; mpsc::Receiver<Bytes>
//            also satisfies the contract.
// ═════════════════════════════════════════════════════════════════════

namespace _ {

template <class R> struct ClientAdapter {
  PromiseResolver<Result<Response>>     m_resolver;
  PromiseResolver<Result<bytes::Bytes>> m_body_resolver;
  bytes::BytesMut                       m_body_buf;
  R                                     m_reader;
  Response                              m_response;

  ClientAdapter(PromiseResolver<Result<Response>> r, PromiseResolver<Result<bytes::Bytes>> br,
                R reader)
      : m_resolver(std::move(r)), m_body_resolver(std::move(br)), m_reader(std::move(reader)) {}

  static int on_response(xHttpCtx *ctx, void *arg) {
    auto *self = static_cast<ClientAdapter *>(arg);
    self->m_response.set_status(static_cast<int>(ctx->status_code));
    self->m_resolver.resolve(Result<Response>(xpp::ok, std::move(self->m_response)));
    return 0;
  }

  static int on_data(const char *data, size_t len, void *arg) {
    auto *self = static_cast<ClientAdapter *>(arg);
    self->m_body_buf.put(reinterpret_cast<const uint8_t *>(data), len);
    return 0;
  }

  static size_t on_read(char *buf, size_t bufsize, void *arg) {
    auto   *self = static_cast<ClientAdapter *>(arg);
    ssize_t n    = self->m_reader.try_read(buf, bufsize);
    if (n > 0) return static_cast<size_t>(n);
    if (n < 0) return bytes::_::kReadFuncPause; // data not ready, curl will retry
    return 0;                                   // EOF
  }

  static void on_done(xHttpCtx *ctx, void *arg) {
    auto *self = static_cast<ClientAdapter *>(arg);
    self->m_body_resolver.resolve(Result<bytes::Bytes>(xpp::ok, self->m_body_buf.freeze()));
    delete self;
  }
};

} // namespace _

// ═════════════════════════════════════════════════════════════════════

inline Promise<Result<Response>> RequestBuilder::send() {
  // TODO: 5 heap allocations per request (2 × async() = 4 allocations for Arc+Node,
  //       + new ClientAdapter).  The body async() is unavoidable because body_p must be
  //       moved into Response before on_response resolves.  Consider merging Arc and
  //       ManualResolveNode into a single allocation at the Promise infrastructure level.
  auto [body_p, body_r] = async<Result<bytes::Bytes>>();
  auto [p, r]           = async<Result<Response>>();

  bool  has_body = m_has_body;
  auto *adapter  = new _::ClientAdapter<bytes::Reader>(std::move(r), std::move(body_r),
                                                       bytes::Reader(std::move(m_body)));

  adapter->m_response.set_body(std::move(body_p));

  xHttpRequestConf conf{};
  conf.url         = m_url.c_str();
  conf.method      = static_cast<xHttpMethod>(m_method);
  conf.on_response = _::ClientAdapter<bytes::Reader>::on_response;
  conf.on_data     = _::ClientAdapter<bytes::Reader>::on_data;
  conf.on_done     = _::ClientAdapter<bytes::Reader>::on_done;

  if (has_body) {
    conf.on_read = _::ClientAdapter<bytes::Reader>::on_read;
  }

  std::vector<std::string> hstrs;
  for (auto &kv : m_headers)
    hstrs.push_back(kv.first + ": " + kv.second);
  std::vector<const char *> hptrs;
  hptrs.reserve(hstrs.size() + 1);
  for (auto &s : hstrs)
    hptrs.push_back(s.c_str());
  hptrs.push_back(nullptr);
  conf.headers = hptrs.data();

  xErrno err = xHttpClientDo(m_client, &conf, adapter);
  if (err != xErrno_Ok) {
    // xHttpClientDo failed — no callbacks will fire, clean up manually.
    adapter->m_resolver.resolve(Result<Response>(xpp::err, Error::request("xHttpClientDo failed")));
    adapter->m_body_resolver.resolve(
      Result<bytes::Bytes>(xpp::err, Error::request("xHttpClientDo failed")));
    delete adapter;
  }
  return std::move(p);
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_CLIENT_H
