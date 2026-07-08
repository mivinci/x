/*
 * client_test.cpp — Tests for xpp::http::Client.
 */
#include <gtest/gtest.h>
#include <xpp/http/client.h>

TEST(HttpClientTest, CreateClient) {
  xpp::http::Client c = xpp::http::Client::create();
  SUCCEED();
}

TEST(HttpClientTest, RequestBuilderChaining) {
  xpp::http::Client c = xpp::http::Client::create();
  auto b = c.get("https://example.com")
              .header("Accept", "text/html")
              .header("X-Custom", "value")
              .body(std::vector<uint8_t>{1, 2, 3});
  SUCCEED();
}
