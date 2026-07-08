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
 * The entry point:
 *
 *   auto client = Client::create();
 *   auto resp = client.get(url).send().await().unwrap();
 *   auto text = resp.text().await().unwrap();
 *
 * For bodies: .body(vector<uint8_t>{...}) for static data, or pass
 * any TryReader (like mpsc::Receiver<Bytes>) for streaming uploads.
 *
 * Aligned with reqwest API.
 */

#ifndef XPP_HTTP_CLIENT_H
#define XPP_HTTP_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <xpp/bytes/bytes_mut.h>
#include <xpp/handle.h>
#include <xpp/http/body.h>
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

  /// Set request body from a vector (static data — auto-wrapped in SliceReader).
  RequestBuilder &body(std::vector<uint8_t> data) {
    return body_impl(SliceReader(data), std::move(data));
  }

  /// Set request body from a TryReader (streaming upload).
  ///
  /// @tparam Reader  Any type with ssize_t try_read(char*, size_t).
  ///                 SliceReader and mpsc::Receiver<Bytes> are built-in.
  template <class Reader>
  RequestBuilder &body(Reader &&reader) {
    return body_impl(std::forward<Reader>(reader), std::vector<uint8_t>{});
  }

  Promise<Result<Response>> send();

private:
  friend class Client;
  xHttpClient              m_client;
  Method                   m_method;
  std::string              m_url;
  std::vector<std::string> m_headers;
  std::vector<uint8_t>     m_body_data; // owns the data (for SliceReader lifetime)

  // Type-erased reader: holds either SliceReader or user-supplied Reader.
  // The reader is stored inline in a buffer — the adapter is created
  // in send() and owned by the heap-allocated HttpAdapter.
  struct ReaderHolder {
    // Enough space for any reasonable reader (SliceReader ~24B, mpsc receiver ~80B).
    // We use a fixed-size buffer to avoid heap allocation.
    alignas(64) char storage[128];
    ssize_t (*try_read)(char *, size_t, void *);  // forwarded from HttpAdapter

    template <class R>
    void emplace(R &&r) {
      static_assert(sizeof(R) <= sizeof(storage),
                    "Reader type too large — use a smaller reader or heap-allocated");
      using DecayedR = typename std::decay<R>::type;
      new (storage) DecayedR(std::forward<R>(r));
      try_read = [](char *buf, size_t cap, void *self) -> ssize_t {
        return static_cast<DecayedR *>(self)->try_read(buf, cap);
      };
    }
  };

  ReaderHolder m_reader; // set by body_impl, read by send()

  RequestBuilder(xHttpClient c, Method m, std::string url)
      : m_client(c), m_method(m), m_url(std::move(url)) {}

  template <class Reader>
  RequestBuilder &body_impl(Reader &&reader, std::vector<uint8_t> data) {
    m_body_data = std::move(data);
    m_reader.emplace(std::forward<Reader>(reader));
    return *this;
  }
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

  // Reader storage — points into RequestBuilder::m_reader.storage.
  // Lifetime: the reader lives on the caller's stack (RequestBuilder).
  // Curl consumes the body synchronously during connect / event loop
  // iteration, so the reader is drained before the RequestBuilder
  // goes out of scope.  For streaming bodies, consider heap-allocating
  // the reader separately.
  void   *m_reader_storage;
  ssize_t (*m_try_read)(char *, size_t, void *);

  HttpAdapter(PromiseResolver<Result<Response>> r,
              PromiseResolver<Result<bytes::Bytes>> br,
              void *reader_storage,
              ssize_t (*try_read)(char *, size_t, void *))
      : m_resolver(std::move(r)), m_body_resolver(std::move(br)),
        m_reader_storage(reader_storage), m_try_read(try_read) {}

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

  static size_t on_read(char *buf, size_t bufsize, void *arg) {
    auto  *self  = static_cast<HttpAdapter *>(arg);
    ssize_t n    = self->m_try_read(buf, bufsize, self->m_reader_storage);
    if (n > 0) return static_cast<size_t>(n);
    if (n < 0) return _::kReadFuncPause;  // data not ready, curl will retry
    return 0;                               // EOF
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
  auto *adapter         = new _::HttpAdapter(
      std::move(r), std::move(body_r),
      m_reader.storage, m_reader.try_read);

  adapter->m_response.set_body(std::move(body_p));

  xHttpRequestConf conf{};
  conf.url         = m_url.c_str();
  conf.method      = static_cast<xHttpMethod>(m_method);
  conf.on_response = _::HttpAdapter::on_response;
  conf.on_data     = _::HttpAdapter::on_data;
  conf.on_done     = _::HttpAdapter::on_done;

  // Wire up body reader if one was set.
  if (m_reader.try_read) {
    conf.on_read    = _::HttpAdapter::on_read;
    // Set content_length for static bodies (not chunked).
    // For streaming bodies (mpsc), we probe: SliceReader returns 0
    // for the first try_read call after EOF, but we can't distinguish
    // at config time.  We default to 0 (= chunked transfer-encoding)
    // which works for both static and streaming bodies.
    conf.content_length = 0;
  }

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
