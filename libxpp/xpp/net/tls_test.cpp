/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tls_test.cpp — Tests for xpp::net::TlsConfig and TlsContext.
 */

#include <gtest/gtest.h>
#include <xpp/net/tls.h>

using xpp::net::TlsConfig;
using xpp::net::TlsContext;

TEST(TlsContextTest, ClientContextIsValid) {
  // Client config (system CA, no cert) should construct successfully.
  TlsContext ctx(TlsConfig::client());
  EXPECT_TRUE(ctx.is_valid());
  EXPECT_TRUE(static_cast<bool>(ctx));
}

TEST(TlsContextTest, ServerContextMissingCertFails) {
  // Server config requires valid cert/key files. Non-existent paths should fail.
  TlsContext ctx(TlsConfig::server("nonexistent.pem", "nonexistent.key"));
  EXPECT_FALSE(ctx.is_valid());
}

TEST(TlsContextTest, MoveSemantics) {
  TlsContext ctx(TlsConfig::client());
  ASSERT_TRUE(ctx.is_valid());

  TlsContext moved = std::move(ctx);
  EXPECT_TRUE(moved.is_valid());
  EXPECT_FALSE(ctx.is_valid()); // moved-from is invalid
}

TEST(TlsConfigTest, BuilderSetters) {
  auto conf =
    TlsConfig::client().with_cert("/tmp/cert.pem").with_key("/tmp/key.pem").with_skip_verify(true);
  EXPECT_EQ(std::string(conf.cert()), "/tmp/cert.pem");
  EXPECT_EQ(std::string(conf.key()), "/tmp/key.pem");
  EXPECT_TRUE(conf.skip_verify());
}

TEST(TlsConfigTest, ClientInsecure) {
  TlsConfig conf = TlsConfig::client_insecure();
  EXPECT_TRUE(conf.skip_verify());
}
