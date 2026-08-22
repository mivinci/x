/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client.h — xpp::http::Client + ClientBuilder.
 *
 * Wraps libx/x/http/client.h's `xHttpClient` C API. `Client::send(Request)`
 * returns `Promise<http::Result<Response>>` — the push-based C callbacks
 * (`on_response` / `on_data` / `on_done`) are bridged to the pull-based
 * `Body` via an `xpp::sync::mpsc::channel<Bytes>(64)` (SendAdapter).
 *
 * Mirrors reqwest::Client / hyper::Client. C++11-compatible. Header-only.
 */

#ifndef XPP_HTTP_CLIENT_H
#define XPP_HTTP_CLIENT_H

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <xpp/http/body.h>
#include <xpp/http/error.h>
#include <xpp/http/header.h>
#include <xpp/http/method.h>
#include <xpp/http/request.h>
#include <xpp/http/response.h>
#include <xpp/http/status.h>
#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/string.h>
#include <xpp/sync/mpsc.h>
#include <xpp/vec.h>

#include <x/base/error.h>  // xErrno
#include <x/http/client.h> // libx C API

namespace xpp {
namespace http {

class ClientBuilder;

/**
 * @brief HTTP client wrapping libx's `xHttpClient`.
 *
 * A `Client` owns a curl multi handle bound to the current `EventLoop`.
 * All requests submitted through a `Client` share the client's defaults
 * (redirect policy, timeouts, proxy, user-agent, TLS) — per-request
 * overrides are set on `RequestBuilder`, not on `Client`.
 *
 * Move-only. `Client::builder()` is the construction entry point.
 */
class Client {
public:
  Client() = default;
  ~Client() {
    if (m_client) xHttpClientDestroy(m_client);
  }

  Client(Client &&o) noexcept : m_client(o.m_client) {
    o.m_client = nullptr;
  }
  Client &operator=(Client &&o) noexcept {
    if (this != &o) {
      if (m_client) xHttpClientDestroy(m_client);
      m_client   = o.m_client;
      o.m_client = nullptr;
    }
    return *this;
  }
  Client(const Client &)            = delete;
  Client &operator=(const Client &) = delete;

  /** @brief Entry point for fluent construction. */
  static ClientBuilder builder();

  /**
   * @brief Send a fully-constructed `Request` asynchronously.
   *
   * Returns a `Promise` that resolves as soon as the response headers
   * arrive (reqwest semantics): `Ok(Response)` with a live, streamed
   * `Body`, or `Err(http::Error)` for transport failures before headers
   * (connect/DNS/timeout) and 4xx/5xx statuses. The `Body` is backed by
   * an mpsc channel with backpressure — read it via `Response::bytes()` /
   * `text()` / `body().read()`. If the transfer fails mid-body, the body
   * read reports the error (instead of a truncated EOF).
   */
  Promise<http::Result<Response>> send(Request req);

private:
  friend class ClientBuilder;
  explicit Client(xHttpClient c) : m_client(c) {}

  xHttpClient m_client = nullptr;
};

/**
 * @brief Fluent builder for `Client`.
 *
 * Chain configurators, then call `build()` to get `Result<Client>`.
 *
 * @code
 *   auto client = xpp::http::Client::builder()
 *                   .timeout(30000)
 *                   .user_agent("myapp/1.0")
 *                   .build()
 *                   .unwrap();
 * @endcode
 */
class ClientBuilder {
public:
  ClientBuilder()                                 = default;
  ClientBuilder(ClientBuilder &&) noexcept        = default;
  ClientBuilder &operator=(ClientBuilder &&)      = default;
  ClientBuilder(const ClientBuilder &)            = delete;
  ClientBuilder &operator=(const ClientBuilder &) = delete;

  /* ── Timeouts (milliseconds) ──────────────────────────────────── */

  /** @brief Total transfer timeout per request in ms. 0 = no limit. */
  ClientBuilder &timeout(uint64_t ms) {
    m_timeout_ms = static_cast<long>(ms);
    return *this;
  }

  /** @brief Connect-phase-only timeout in ms. 0 = default (~300s). */
  ClientBuilder &connect_timeout(uint64_t ms) {
    m_connect_timeout_ms = static_cast<long>(ms);
    return *this;
  }

  /**
   * @brief Read timeout (time between body chunks) in ms.
   *
   * The transfer aborts if no response body bytes arrive for this long
   * (idle detection, implemented with an event-loop timer in libx — curl's
   * CURLOPT_LOW_SPEED_TIME is unreliable for mid-transfer stalls). 0 = off.
   */
  ClientBuilder &read_timeout(uint64_t ms) {
    m_read_timeout_ms = static_cast<long>(ms);
    return *this;
  }

  /* ── Redirects ────────────────────────────────────────────────── */

  /** @brief Set the redirect policy. 0 = never follow, non-zero = follow. */
  ClientBuilder &redirect(int policy) {
    m_follow_location = (policy != 0) ? 1 : 0;
    return *this;
  }

  /** @brief Max redirects to follow. 0 = follow infinitely. */
  ClientBuilder &max_redirects(long n) {
    m_max_redirects = n;
    return *this;
  }

  /* ── Identity / proxy ──────────────────────────────────────────── */

  /** @brief Default `User-Agent` header for all requests. */
  ClientBuilder &user_agent(String ua) {
    m_user_agent = std::move(ua);
    return *this;
  }
  ClientBuilder &user_agent(const char *ua) {
    if (ua) m_user_agent = String::from_utf8(ua).unwrap();
    return *this;
  }

  /** @brief Proxy URL (e.g. "http://host:port", "socks5://host:port"). */
  ClientBuilder &proxy(String url) {
    m_proxy = std::move(url);
    return *this;
  }
  ClientBuilder &proxy(const char *url) {
    if (url) m_proxy = String::from_utf8(url).unwrap();
    return *this;
  }

  /** @brief Comma-separated host patterns that bypass the proxy. */
  ClientBuilder &no_proxy(String hosts) {
    m_no_proxy = std::move(hosts);
    return *this;
  }
  ClientBuilder &no_proxy(const char *hosts) {
    if (hosts) m_no_proxy = String::from_utf8(hosts).unwrap();
    return *this;
  }

  /* ── Headers ──────────────────────────────────────────────────── */

  /** @brief Add a default header applied to every request. */
  ClientBuilder &header(String key, String value) {
    m_default_headers.insert(std::move(key), std::move(value));
    return *this;
  }
  ClientBuilder &header(const char *key, const char *value) {
    XPP_ASSERT(key != nullptr && value != nullptr, "ClientBuilder::header(nullptr)");
    m_default_headers.insert(String::from_utf8(key).unwrap(), String::from_utf8(value).unwrap());
    return *this;
  }

  /** @brief Set `Authorization: Bearer <token>` default header. */
  ClientBuilder &bearer_auth(String token) {
    String v = String::from_utf8("Bearer ").unwrap();
    v.push_str(token);
    m_default_headers.insert(String::from_utf8("Authorization").unwrap(), std::move(v));
    return *this;
  }

  /** @brief Set `Authorization: Basic <base64(user:pass)>` default header. */
  ClientBuilder &basic_auth(String user, String password) {
    m_default_headers.insert(String::from_utf8("Authorization").unwrap(),
                             _::basic_auth_value(std::move(user), std::move(password)));
    return *this;
  }

  /* ── TLS ──────────────────────────────────────────────────────── */

  /**
   * @brief Skip TLS certificate verification.
   *
   * Dangerous — only use in tests / dev. Sets `tls.skip_verify = 1`.
   */
  ClientBuilder &danger_accept_invalid_certs(bool skip = true) {
    m_tls_skip_verify = skip ? 1 : 0;
    return *this;
  }

  /** @brief CA cert file path for TLS verification. */
  ClientBuilder &tls(String ca_path) {
    m_tls_ca = std::move(ca_path);
    return *this;
  }

  /* ── HTTP version ─────────────────────────────────────────────── */

  /** @brief Force HTTP/1.1 only (no H2 negotiation). */
  ClientBuilder &http1_only() {
    m_http_version = xHttpVersion_H1;
    return *this;
  }

  /** @brief Force HTTP/2 with prior knowledge (no ALPN). */
  ClientBuilder &http2_prior_knowledge() {
    m_http_version = xHttpVersion_H2C;
    return *this;
  }

  /* ── Termination ──────────────────────────────────────────────── */

  /**
   * @brief Construct the `Client`.
   *
   * Returns `Err` if `xHttpClientCreate` fails (no current EventLoop,
   * OOM, or curl_multi_init failure).
   */
  http::Result<Client> build() {
    xHttpClientConf conf = {};

    conf.follow_location    = m_follow_location;
    conf.max_redirects      = m_max_redirects;
    conf.timeout_ms         = m_timeout_ms;
    conf.connect_timeout_ms = m_connect_timeout_ms;
    conf.read_timeout_ms    = m_read_timeout_ms;

    // Convert xpp::String → std::string for C API (null-terminated).
    auto to_std = [](const String &s) -> std::string {
      auto b = s.as_bytes();
      return std::string(reinterpret_cast<const char *>(b.data()), b.size());
    };
    std::string ua_std, px_std, np_std;
    if (!m_user_agent.empty()) ua_std = to_std(m_user_agent);
    if (!m_proxy.empty()) px_std = to_std(m_proxy);
    if (!m_no_proxy.empty()) np_std = to_std(m_no_proxy);
    conf.user_agent = ua_std.empty() ? nullptr : ua_std.c_str();
    conf.proxy      = px_std.empty() ? nullptr : px_std.c_str();
    conf.no_proxy   = np_std.empty() ? nullptr : np_std.c_str();

    conf.http_version = m_http_version;

    // TLS
    xTlsConf    tls_conf = {};
    xTlsConf   *tls_ptr  = nullptr;
    std::string ca_std;
    if (m_tls_skip_verify || !m_tls_ca.empty()) {
      tls_conf.skip_verify = m_tls_skip_verify;
      if (!m_tls_ca.empty()) {
        ca_std      = to_std(m_tls_ca);
        tls_conf.ca = ca_std.c_str();
      }
      tls_ptr = &tls_conf;
    }
    conf.tls = tls_ptr;

    xHttpClient c = xHttpClientCreate(&conf);
    if (!c) {
      return http::Result<Client>(
        xpp::err,
        Error(Error::Kind::Connect, String::from_utf8("xHttpClientCreate failed").unwrap()));
    }
    return http::Result<Client>(xpp::ok, Client(c));
  }

private:
  long         m_timeout_ms         = 0;
  long         m_connect_timeout_ms = 0;
  long         m_read_timeout_ms    = 0;
  int          m_follow_location    = 1;
  long         m_max_redirects      = 10;
  String       m_user_agent;
  String       m_proxy;
  String       m_no_proxy;
  String       m_tls_ca;
  int          m_tls_skip_verify = 0;
  xHttpVersion m_http_version    = xHttpVersion_Default;
  HeaderMap    m_default_headers;
};

inline ClientBuilder Client::builder() {
  return ClientBuilder();
}

/* ── Internal: SendAdapter + C callback trampolines ─────────────── */

namespace _ {

/**
 * @brief Per-request state held by `Client::send`.
 *
 * Owns the mpsc sender (used by `on_data` to push body chunks), the
 * `PromiseResolver` (used by `on_done` to fulfill the returned Promise),
 * and the channel-backed `Body` (moved into the `Response` at `on_done`).
 *
 * `on_response` populates `status` / `headers` / `final_url`.
 * `on_done` closes the channel, constructs the `Response` (moving the
 * body out of the adapter), resolves the promise, and `delete`s the adapter.
 *
 * Lifetime: `new`-allocated in `Client::send`, `delete`-ed in `on_done`.
 */
struct SendAdapter {
  SendAdapter() : xfer_error(Shared<Option<Error>>::make()) {}

  // Option<> wrappers allow default-construction; the actual values
  // are moved in by Client::send() before the request is submitted.
  Option<sync::mpsc::Sender<Bytes>>               tx;
  Option<PromiseResolver<http::Result<Response>>> resolver;
  Body                                            body; // channel-backed response body
  // Shared transfer-error flag: on_done writes it if the body transfer
  // fails after the send promise already resolved (headers arrived). The
  // Response's Body reads it and reports a read error instead of EOF.
  Shared<Option<Error>> xfer_error;
  bool                  discard_body = false; // error response — drop body data
  // Request body pulled by libcurl's read callback (on_read).
  Bytes  req_body;         // moved in by Client::send() when non-empty
  size_t req_body_off = 0; // read cursor — on_read advances it
  // Populated by on_response:
  StatusCode::Value status = StatusCode::Ok;
  HeaderMap         headers;
  Option<String>    final_url;
  bool              headers_done = false;
};

namespace _ {

/// Portable memmem: find @p needle in [hay, hay+hlen). Returns a pointer
/// into @p hay, or NULL. (memmem is not standard C — glibc needs
/// _GNU_SOURCE and Windows lacks it entirely.)
inline const char *find_bytes(const char *hay, size_t hlen, const char *needle, size_t nlen) {
  if (!hay || nlen == 0 || nlen > hlen) return nullptr;
  for (size_t i = 0; i + nlen <= hlen; ++i) {
    size_t j = 0;
    while (j < nlen && hay[i + j] == needle[j])
      ++j;
    if (j == nlen) return hay + i;
  }
  return nullptr;
}

} // namespace _

// Parse raw response headers (NUL-terminated, "\r\n"-separated) into a HeaderMap.
inline HeaderMap parse_raw_headers(const char *raw, size_t len) {
  HeaderMap map;
  if (!raw || len == 0) return map;

  const char *end = raw + len;

  // With redirects followed (CURLOPT_FOLLOWLOCATION), libcurl invokes the
  // header callback once per response in the chain, so the buffer contains
  // one header block per hop:
  //   "HTTP/1.1 301...\r\nLocation: ...\r\n\r\nHTTP/1.1 200 OK\r\n...\r\n\r\n"
  // CURLINFO_RESPONSE_CODE reports the FINAL status, so parse the LAST
  // complete block to keep status and headers consistent.
  const char *p        = raw;
  const char *prev_sep = nullptr;
  const char *last_sep = nullptr;
  while (true) {
    const char *next = _::find_bytes(p, end - p, "\r\n\r\n", 4);
    if (!next) break;
    prev_sep = last_sep;
    last_sep = next;
    p        = next + 4;
  }
  const char *block_start = prev_sep ? prev_sep + 4 : raw;
  const char *block_end   = last_sep ? last_sep : end;

  // Skip the status line (e.g. "HTTP/1.1 200 OK\r\n")
  const char *eol = _::find_bytes(block_start, block_end - block_start, "\r\n", 2);
  if (eol) {
    p = eol + 2;
  } else {
    p = block_start;
  }

  while (p < block_end) {
    const char *line_end = _::find_bytes(p, block_end - p, "\r\n", 2);
    if (!line_end) line_end = block_end;
    if (line_end == p) break; // empty line — end of headers

    const char *colon = _::find_bytes(p, line_end - p, ":", 1);
    if (colon) {
      const char *val_start = colon + 1;
      while (val_start < line_end && (*val_start == ' ' || *val_start == '\t'))
        val_start++;
      size_t key_len = colon - p;
      size_t val_len = line_end - val_start;
      String key     = String::from_utf8(p, key_len).unwrap();
      String value   = String::from_utf8(val_start, val_len).unwrap();
      map.insert(std::move(key), std::move(value));
    }

    if (line_end == block_end) break;
    p = line_end + 2;
  }
  return map;
}

// Build an http::Error from a curl code + status.
inline Error error_from_curl(int curl_code, long status_code, const char *curl_error) {
  String msg;
  if (curl_error && *curl_error) {
    msg = String::from_utf8(curl_error).unwrap();
  } else {
    msg = String::from_utf8("request failed").unwrap();
  }

  // curl succeeded but status is 4xx/5xx → Protocol error with status.
  if (curl_code == 0 && (status_code >= 400 && status_code < 600)) {
    StatusCode::Value sc = static_cast<StatusCode::Value>(static_cast<uint16_t>(status_code));
    return Error(Error::Kind::Protocol, std::move(msg), sc);
  }

  switch (curl_code) {
  case 3:
    return Error(Error::Kind::InvalidUrl, std::move(msg));
  case 6:
    return Error(Error::Kind::Dns, std::move(msg));
  case 7:
    return Error(Error::Kind::Connect, std::move(msg));
  case 28:
    return Error(Error::Kind::Timeout, std::move(msg));
  case 47:
    return Error(Error::Kind::TooManyRedirects, std::move(msg));
  case 35:
  case 51:
  case 58:
  case 60:
  case 64:
    return Error(Error::Kind::Tls, std::move(msg));
  case 5:
    return Error(Error::Kind::Connect, std::move(msg));
  case 23: // CURLE_WRITE_ERROR — our on_data aborted (body buffer overflowed)
    return Error(Error::Kind::Body,
                 String::from_utf8("response body buffer overflowed (consumer too slow)").unwrap());
  default:
    return Error(Error::Kind::Io, std::move(msg));
  }
}

// Pull request body bytes for libcurl's upload. Serves from the adapter's
// req_body buffer until exhausted, then returns 0 (EOF). libcurl may call
// this multiple times — the cursor keeps track of what has been sent.
inline size_t on_read_cb(char *buf, size_t bufsize, void *arg) {
  auto  *adapter   = static_cast<SendAdapter *>(arg);
  size_t remaining = adapter->req_body.size() - adapter->req_body_off;
  size_t n         = remaining < bufsize ? remaining : bufsize;
  if (n == 0) return 0; // EOF
  std::memcpy(buf, adapter->req_body.data() + adapter->req_body_off, n);
  adapter->req_body_off += n;
  return n;
}

inline int on_response_cb(xHttpCtx *ctx, void *arg) {
  auto *adapter = static_cast<SendAdapter *>(arg);
  if (adapter->headers_done) return 0;
  adapter->headers_done = true;
  adapter->status       = static_cast<StatusCode::Value>(static_cast<uint16_t>(ctx->status_code));
  adapter->headers      = parse_raw_headers(ctx->headers, ctx->headers_len);
  if (ctx->url) {
    adapter->final_url = xpp::some(String::from_utf8(ctx->url).unwrap());
  }

  // Transfer already failed (e.g. timeout before any header arrived) —
  // libx invokes on_response as a courtesy on the completion path with
  // status_code == 0. Leave resolution to on_done, which carries the error.
  if (ctx->curl_code != 0) return 0;

  // Resolve the send promise as soon as the response headers arrive
  // (reqwest semantics): the Response carries a live, streamed body that
  // the consumer reads while the transfer is still in flight.
  if (ctx->status_code >= 400 && ctx->status_code < 600) {
    // Error status — resolve Err immediately; the body is discarded.
    adapter->discard_body = true;
    auto err              = error_from_curl(0, ctx->status_code, ctx->curl_error);
    adapter->resolver.unwrap().resolve(http::Result<Response>(xpp::err, std::move(err)));
  } else {
    Body            body = std::move(adapter->body);
    ResponseBuilder rb   = Response::builder();
    rb.status(adapter->status);
    if (adapter->final_url.is_some()) rb.url(adapter->final_url.unwrap());
    for (auto it = adapter->headers.begin(); it != adapter->headers.end(); ++it) {
      auto kv = *it;
      rb.header(kv.first, kv.second);
    }
    Response resp = rb.body(std::move(body));
    adapter->resolver.unwrap().resolve(http::Result<Response>(xpp::ok, std::move(resp)));
  }
  adapter->resolver = none; // consumed — on_done must not resolve again
  return 0;
}

inline int on_data_cb(const char *data, size_t len, void *arg) {
  auto *adapter = static_cast<SendAdapter *>(arg);
  if (adapter->discard_body) return 0;
  Bytes chunk = Bytes::copy(data, len);
  auto  r     = adapter->tx.unwrap().try_send(std::move(chunk));
  if (r.is_err()) {
    // Channel full — pause the transfer (backpressure) instead of aborting.
    // libx buffers this chunk; Body's on_drain hook resumes the transfer
    // once the consumer frees a channel slot.
    return 1;
  }
  return 0;
}

inline void on_done_cb(xHttpCtx *ctx, void *arg) {
  auto *adapter = static_cast<SendAdapter *>(arg);

  // Close the channel — signals EOF to Body::read on the consumer side
  // (or a transfer error via xfer_error below).
  adapter->tx.unwrap().close();

  bool already_resolved = !adapter->resolver.is_some();

  if (!already_resolved) {
    // on_response never ran — the request failed before headers arrived
    // (connect/DNS/timeout). Resolve the promise with the error.
    auto err = error_from_curl(ctx->curl_code, ctx->status_code, ctx->curl_error);
    adapter->resolver.unwrap().resolve(http::Result<Response>(xpp::err, std::move(err)));
  } else if (ctx->curl_code != 0) {
    // Headers arrived (promise resolved Ok) but the body transfer failed
    // midway (e.g. connection reset). Surface the error to the body
    // consumer instead of a silent truncated EOF.
    if (adapter->xfer_error.get()) {
      *adapter->xfer_error =
        xpp::some(error_from_curl(ctx->curl_code, ctx->status_code, ctx->curl_error));
    }
  }

  delete adapter;
}

} // namespace _

/* ── Client::send implementation ────────────────────────────────── */

inline Promise<http::Result<Response>> Client::send(Request req) {
  // 0. Extract the request body (if any).
  //    libcurl's read callback is synchronous — a streaming (channel)
  //    request body cannot be fed from within the event loop without
  //    blocking it. Reject with a clear error instead of silently
  //    dropping the body.
  Body req_body = std::move(req).into_body();
  if (req_body.is_channel()) {
    return xpp::resolve(http::Result<Response>(
      xpp::err,
      Error(Error::Kind::Body, String::from_utf8("streaming (channel) request bodies are not "
                                                 "supported yet")
                                 .unwrap())));
  }
  Bytes req_body_bytes = req_body.into_once_bytes();

  // 0b. Validate the URL and method before allocating anything.
  if (req.url().empty()) {
    return xpp::resolve(http::Result<Response>(
      xpp::err, Error(Error::Kind::InvalidUrl, String::from_utf8("empty request URL").unwrap())));
  }
  if (req.method() == Method::Connect) {
    // libx/curl has no CONNECT request path — fail loudly instead of
    // silently sending a GET.
    return xpp::resolve(http::Result<Response>(
      xpp::err, Error(Error::Kind::InvalidUrl,
                      String::from_utf8("CONNECT method is not supported").unwrap())));
  }

  // 1. Create mpsc channel for body streaming. The producer (libcurl's
  //    on_data) and consumer run on the same event loop thread; curl can
  //    deliver several chunks per socket event, so the channel needs
  //    headroom beyond the per-iteration drain. 256 × ~16KB ≈ 4MB of
  //    buffering worst case. If it still fills, on_data aborts and the
  //    request fails with a Kind::Body error (see on_data_cb).
  auto channel_pair = sync::mpsc::channel<Bytes>(256);
  auto tx           = std::move(channel_pair.first);
  auto rx           = std::move(channel_pair.second);

  // 2. Create the async result holder (Promise + Resolver).
  auto pr       = xpp::async<http::Result<Response>>();
  auto promise  = std::move(pr.first);
  auto resolver = std::move(pr.second);

  // 3. Allocate SendAdapter — on_done will `delete` it.
  _::SendAdapter *adapter = new _::SendAdapter();
  adapter->tx             = xpp::some(std::move(tx));
  adapter->resolver       = xpp::some(std::move(resolver));

  // The response body drains the channel via Body::read; each successful
  // recv frees a slot, so resume the transfer (backpressure) there. Safe:
  // the hook is only reachable while the transfer is in flight — on_done
  // closes the channel and resolves the promise before deleting the
  // adapter, and the consumer stops reading once it sees EOF/error.
  xHttpClient     client_h    = m_client;
  _::SendAdapter *adapter_ptr = adapter;
  auto            xfer_error  = Shared<Option<Error>>::make();
  adapter->xfer_error         = xfer_error;
  adapter->body               = Body::from_channel(
    std::move(rx), [client_h, adapter_ptr]() { xHttpClientResume(client_h, adapter_ptr); },
    xfer_error);

  // 4. Build xHttpRequestConf from the Request.
  //    Keep url_std + header_std_strings alive for the duration of
  //    xHttpClientDo. libx copies url + headers internally before
  //    returning, so the local stack variables are safe.
  auto        url_bytes = req.url().as_bytes();
  std::string url_std(reinterpret_cast<const char *>(url_bytes.data()), url_bytes.size());

  xHttpMethod method = xHttpMethod_GET;
  switch (req.method()) {
  case Method::Get:
    method = xHttpMethod_GET;
    break;
  case Method::Post:
    method = xHttpMethod_POST;
    break;
  case Method::Put:
    method = xHttpMethod_PUT;
    break;
  case Method::Delete:
    method = xHttpMethod_DELETE;
    break;
  case Method::Patch:
    method = xHttpMethod_PATCH;
    break;
  case Method::Head:
    method = xHttpMethod_HEAD;
    break;
  case Method::Options:
    method = xHttpMethod_OPTIONS;
    break;
  case Method::Trace:
    method = xHttpMethod_TRACE;
    break;
  case Method::Connect:
    break; // unreachable — rejected in step 0b
  }

  // Build "Key: Value" array for headers.
  Vec<std::string>  header_std_strings;
  Vec<const char *> header_ptrs;
  const HeaderMap  &req_headers = req.headers();
  for (auto it = req_headers.begin(); it != req_headers.end(); ++it) {
    auto        kv = *it;
    auto        k  = kv.first.as_bytes();
    auto        v  = kv.second.as_bytes();
    std::string h(reinterpret_cast<const char *>(k.data()), k.size());
    h += ": ";
    h += std::string(reinterpret_cast<const char *>(v.data()), v.size());
    header_std_strings.push(std::move(h));
    // Address of the just-pushed string — stable as long as we don't
    // resize/capacity-change after pushing (Vec growth invalidates).
    header_ptrs.push(header_std_strings[header_std_strings.len() - 1].c_str());
  }
  header_ptrs.push(nullptr);

  xHttpRequestConf conf = {};
  conf.method           = method;
  conf.url              = url_std.c_str();
  conf.headers          = header_ptrs.data();
  conf.on_response      = _::on_response_cb;
  conf.on_data          = _::on_data_cb;
  conf.on_done          = _::on_done_cb;

  // Request body (if any): served to libcurl via the synchronous on_read
  // callback. content_length > 0 → fixed Content-Length header.
  if (!req_body_bytes.empty()) {
    adapter->req_body   = std::move(req_body_bytes);
    conf.on_read        = _::on_read_cb;
    conf.content_length = adapter->req_body.size();
  }

  // 5. Submit the request.
  xErrno rc = xHttpClientDo(m_client, &conf, adapter);

  if (rc != xErrno_Ok) {
    // Submission failed — clean up and resolve with error.
    delete adapter;
    return xpp::resolve(http::Result<Response>(
      xpp::err, Error(Error::Kind::Io, String::from_utf8("xHttpClientDo failed").unwrap())));
  }

  return promise;
}

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_CLIENT_H
