/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tls.h - xpp::net::TlsConfig and TlsContext: RAII TLS configuration.
 *
 * TlsConfig is a thin builder around xTlsConf (POD with const char*
 * pointers; the caller owns the string storage). TlsContext is RAII
 * over xTlsCtx — created by xTlsCtxCreate, destroyed by xTlsCtxDestroy.
 *
 * Pass a TlsContext* to TcpConn::connect() to enable TLS — libx's
 * xTcpConnect does the handshake transparently.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_NET_TLS_H
#define XPP_NET_TLS_H

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include <xpp/option.h>
#include <xpp/result.h>

#include <x/base/error.h>
#include <x/net/tls.h>

namespace xpp {
namespace net {

/* ── TlsConfig ────────────────────────────────────────────────────── */

/**
 * @brief Builder for xTlsConf.
 *
 * Owns the string storage (cert path, key path, CA path, key password,
 * ALPN protocols) so the resulting xTlsConf remains valid for the
 * lifetime of the TlsConfig. Use client() or server() factories, or
 * build manually via the setters.
 *
 * Zero-alloc on the libx side — xTlsConf is a POD of pointers.
 */
class TlsConfig {
public:
  /** @brief Client defaults: system CA bundle, peer verification on. */
  static TlsConfig client() {
    TlsConfig c;
    c.m_conf.skip_verify = 0;
    return c;
  }

  /** @brief Client that skips peer verification (e.g. self-signed certs). */
  static TlsConfig client_insecure() {
    TlsConfig c;
    c.m_conf.skip_verify = 1;
    return c;
  }

  /** @brief Server config with certificate and private key paths. */
  static TlsConfig server(const char *cert_path, const char *key_path) {
    TlsConfig c;
    c.m_cert      = cert_path;
    c.m_key       = key_path;
    c.m_conf.cert = c.m_cert.c_str();
    c.m_conf.key  = c.m_key.c_str();
    return c;
  }

  /** @brief Server config with certificate, key, and an optional CA (mTLS). */
  static TlsConfig server(const char *cert_path, const char *key_path, const char *ca_path) {
    TlsConfig c = server(cert_path, key_path);
    if (ca_path) {
      c.m_ca      = ca_path;
      c.m_conf.ca = c.m_ca.c_str();
    }
    return c;
  }

  TlsConfig()  = default;
  ~TlsConfig() = default;

  TlsConfig(TlsConfig &&o) noexcept
      : m_cert(std::move(o.m_cert)), m_key(std::move(o.m_key)), m_ca(std::move(o.m_ca)),
        m_key_password(std::move(o.m_key_password)), m_alpn(std::move(o.m_alpn)), m_conf(o.m_conf) {
    rebuild_alpn_ptrs();
    fixup_ptrs();
    o.m_conf = xTlsConf{};
  }

  TlsConfig &operator=(TlsConfig &&o) noexcept {
    if (this != &o) {
      m_cert         = std::move(o.m_cert);
      m_key          = std::move(o.m_key);
      m_ca           = std::move(o.m_ca);
      m_key_password = std::move(o.m_key_password);
      m_alpn         = std::move(o.m_alpn);
      m_conf         = o.m_conf;
      rebuild_alpn_ptrs();
      fixup_ptrs();
      o.m_conf = xTlsConf{};
    }
    return *this;
  }

  TlsConfig(const TlsConfig &)            = delete;
  TlsConfig &operator=(const TlsConfig &) = delete;

  /* ── Setters (builder) ─────────────────────────────────────────── */

  /** @brief Set the certificate (PEM) file path. */
  TlsConfig &with_cert(const char *path) & {
    m_cert      = path ? path : "";
    m_conf.cert = m_cert.c_str();
    return *this;
  }
  TlsConfig &&with_cert(const char *path) && {
    with_cert(path);
    return std::move(*this);
  }
  /** @brief Set the private key (PEM) file path. */
  TlsConfig &with_key(const char *path) & {
    m_key      = path ? path : "";
    m_conf.key = m_key.c_str();
    return *this;
  }
  TlsConfig &&with_key(const char *path) && {
    with_key(path);
    return std::move(*this);
  }
  /** @brief Set the CA certificate (PEM) file path. */
  TlsConfig &with_ca(const char *path) & {
    m_ca      = path ? path : "";
    m_conf.ca = m_ca.c_str();
    return *this;
  }
  TlsConfig &&with_ca(const char *path) && {
    with_ca(path);
    return std::move(*this);
  }
  /** @brief Set the password for the private key. */
  TlsConfig &with_key_password(const char *password) & {
    m_key_password      = password ? password : "";
    m_conf.key_password = m_key_password.c_str();
    return *this;
  }
  TlsConfig &&with_key_password(const char *password) && {
    with_key_password(password);
    return std::move(*this);
  }
  /** @brief Set ALPN protocol list (e.g. {"h2", "http/1.1"}). */
  TlsConfig &with_alpn(const std::vector<std::string> &protocols) & {
    m_alpn = protocols;
    rebuild_alpn_ptrs();
    return *this;
  }
  TlsConfig &&with_alpn(const std::vector<std::string> &protocols) && {
    with_alpn(protocols);
    return std::move(*this);
  }
  /** @brief Skip peer certificate verification (e.g. for self-signed certs). */
  TlsConfig &with_skip_verify(bool skip) & {
    m_conf.skip_verify = skip ? 1 : 0;
    return *this;
  }
  TlsConfig &&with_skip_verify(bool skip) && {
    with_skip_verify(skip);
    return std::move(*this);
  }

  /* ── Accessors ─────────────────────────────────────────────────── */

  /** @brief Raw xTlsConf pointer (valid for the lifetime of this TlsConfig). */
  const xTlsConf *raw() const {
    return &m_conf;
  }
  /** @brief Certificate file path (may be null). */
  const char *cert() const {
    return m_conf.cert;
  }
  /** @brief Private key file path (may be null). */
  const char *key() const {
    return m_conf.key;
  }
  /** @brief CA certificate file path (may be null). */
  const char *ca() const {
    return m_conf.ca;
  }
  /** @brief Key password (may be null). */
  const char *key_password() const {
    return m_conf.key_password;
  }
  /** @brief Whether verification is skipped. */
  bool skip_verify() const {
    return m_conf.skip_verify != 0;
  }

private:
  std::string               m_cert;
  std::string               m_key;
  std::string               m_ca;
  std::string               m_key_password;
  std::vector<std::string>  m_alpn;
  std::vector<const char *> m_alpn_ptrs;
  xTlsConf                  m_conf{};

  void fixup_ptrs() {
    m_conf.cert         = m_cert.empty() ? nullptr : m_cert.c_str();
    m_conf.key          = m_key.empty() ? nullptr : m_key.c_str();
    m_conf.ca           = m_ca.empty() ? nullptr : m_ca.c_str();
    m_conf.key_password = m_key_password.empty() ? nullptr : m_key_password.c_str();
  }

  void rebuild_alpn_ptrs() {
    m_alpn_ptrs.clear();
    if (!m_alpn.empty()) {
      m_alpn_ptrs.reserve(m_alpn.size() + 1);
      for (auto &p : m_alpn)
        m_alpn_ptrs.push_back(p.c_str());
      m_alpn_ptrs.push_back(nullptr);
    }
    m_conf.alpn = m_alpn_ptrs.empty() ? nullptr : m_alpn_ptrs.data();
  }
};

/* ── TlsContext ───────────────────────────────────────────────────── */

/**
 * @brief RAII wrapper around xTlsCtx.
 *
 * Created from a TlsConfig (or raw xTlsConf). The underlying context
 * is shared across all connections on a listener or connector — pass
 * a TlsContext* to TcpConn::connect() or TcpListener::bind().
 *
 * Move-only. NULL on construction failure (check is_valid()).
 */
class TlsContext {
public:
  /** @brief Create a TLS context from a TlsConfig. NULL on failure. */
  explicit TlsContext(const TlsConfig &conf) : m_ctx(xTlsCtxCreate(conf.raw())) {}

  /** @brief Create a TLS context from a raw xTlsConf. NULL on failure. */
  explicit TlsContext(const xTlsConf *conf) : m_ctx(xTlsCtxCreate(conf)) {}

  TlsContext() : m_ctx(nullptr) {}

  ~TlsContext() {
    if (m_ctx) xTlsCtxDestroy(m_ctx);
  }

  TlsContext(TlsContext &&o) noexcept : m_ctx(o.m_ctx) {
    o.m_ctx = nullptr;
  }
  TlsContext &operator=(TlsContext &&o) noexcept {
    if (this != &o) {
      if (m_ctx) xTlsCtxDestroy(m_ctx);
      m_ctx   = o.m_ctx;
      o.m_ctx = nullptr;
    }
    return *this;
  }
  TlsContext(const TlsContext &)            = delete;
  TlsContext &operator=(const TlsContext &) = delete;

  /** @brief Hot-reload certificates. Returns 0 on success, -1 on failure. */
  int reload(const TlsConfig &conf) {
    return xTlsCtxReload(m_ctx, conf.raw());
  }

  /** @brief Raw xTlsCtx handle (may be nullptr if construction failed). */
  xTlsCtx raw() const {
    return m_ctx;
  }
  /** @brief Returns true if the context was created successfully. */
  bool is_valid() const {
    return m_ctx != nullptr;
  }
  /** @brief Same as is_valid(). */
  explicit operator bool() const {
    return is_valid();
  }

private:
  xTlsCtx m_ctx;
};

} // namespace net
} // namespace xpp

#endif // XPP_NET_TLS_H
