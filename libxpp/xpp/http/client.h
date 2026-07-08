/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client.h - xpp::http::Client: Promise-based async HTTP client.
 *
 * Wraps libx xHttpClient via OwnedHandle. Request pipeline via
 * ClientAdapter (heap, self-delete). Response resolved at header time.
 *
 * Body data flows through a single mpsc channel:
 *   on_data → try_send(chunk) → [channel] → chunk() / text() / bytes()
 *   on_done → close channel
 *
 * No buffering in the adapter — the channel is the single source of truth.
 *
 * The entry point:
 *
 *   auto client  = Client::builder().build();
 *   auto resp    = client.get(url).send().await().unwrap();
 *   // Buffered:  auto text = resp.text().await().unwrap();
 *   // Streaming: while (auto c = resp.chunk().await()) { ... }
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
#include <xpp/bytes/reader.h>
#include <xpp/handle.h>
#include <xpp/http/error.h>
#include <xpp/http/response.h>
#include <xpp/promise.h>
#include <xpp/sync/mpsc.h>

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
// @tparam R  TryRead type for request body (bytes::Reader for static).
//
// Body data flows through a single mpsc channel:
//   on_data → try_send(chunk) → channel → Response::chunk()/text()/bytes()
//   on_done → close channel
//
// No buffering — the channel is the single source of truth.
// ═════════════════════════════════════════════════════════════════════

namespace _ {

template <class R> struct ClientAdapter {
  PromiseResolver<Result<Response>> m_resolver;
  R                                 m_reader;
  sync::mpsc::Sender<bytes::Bytes>  m_body_tx;
  Response                          m_response;

  ClientAdapter(PromiseResolver<Result<Response>> r,
                R reader,
                sync::mpsc::Sender<bytes::Bytes> tx)
      : m_resolver(std::move(r)), m_reader(std::move(reader)),
        m_body_tx(std::move(tx)) {}

  static int on_response(xHttpCtx *ctx, void *arg) {
    auto *self = static_cast<ClientAdapter *>(arg);
    self->m_response.set_status(static_cast<int>(ctx->status_code));
    self->m_resolver.resolve(Result<Response>(xpp::ok, std::move(self->m_response)));
    return 0;
  }

  static int on_data(const char *data, size_t len, void *arg) {
    auto *self = static_cast<ClientAdapter *>(arg);
    self->m_body_tx.try_send(bytes::Bytes::from(std::vector<uint8_t>(
      reinterpret_cast<const uint8_t *>(data),
      reinterpret_cast<const uint8_t *>(data) + len)));
    return 0;
  }

  static size_t on_read(char *buf, size_t bufsize, void *arg) {
    auto   *self = static_cast<ClientAdapter *>(arg);
    ssize_t n    = self->m_reader.try_read(buf, bufsize);
    if (n > 0) return static_cast<size_t>(n);
    if (n < 0) return bytes::_::kReadFuncPause;
    return 0;
  }

  static void on_done(xHttpCtx *ctx, void *arg) {
    auto *self = static_cast<ClientAdapter *>(arg);
    self->m_body_tx.close();
    delete self;
  }
};

} // namespace _

// ═════════════════════════════════════════════════════════════════════

inline Promise<Result<Response>> RequestBuilder::send() {
  // Body channel — 8 chunks of back-pressure.
  auto [body_tx, body_rx] = sync::mpsc::channel<bytes::Bytes>(8);

  auto [p, r] = async<Result<Response>>();

  bool  has_body = m_has_body;
  auto *adapter  = new _::ClientAdapter<bytes::Reader>(
      std::move(r),
      bytes::Reader(std::move(m_body)),
      std::move(body_tx));

  adapter->m_response.set_body_channel(std::move(body_rx));

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
    adapter->m_body_tx.close();
    adapter->m_resolver.resolve(
      Result<Response>(xpp::err, Error::request("xHttpClientDo failed")));
    delete adapter;
  }
  return std::move(p);
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_CLIENT_H
