/*
 * Unit tests for xpp::http::StatusCode.
 */

#include <gtest/gtest.h>
#include <xpp/http/status.h>

using namespace xpp;
using namespace xpp::http;

/* ───────────────────────────────────────────────────────────────────
 *  Classification predicates
 * ─────────────────────────────────────────────────────────────────── */

TEST(StatusClassification, Informational) {
  EXPECT_TRUE(is_informational(StatusCode::Continue));
  EXPECT_TRUE(is_informational(StatusCode::SwitchingProtocols));
  EXPECT_TRUE(is_informational(StatusCode::EarlyHints));
  EXPECT_FALSE(is_informational(StatusCode::Ok));
  EXPECT_FALSE(is_informational(StatusCode::NotFound));
}

TEST(StatusClassification, Success) {
  EXPECT_TRUE(is_success(StatusCode::Ok));
  EXPECT_TRUE(is_success(StatusCode::Created));
  EXPECT_TRUE(is_success(StatusCode::NoContent));
  EXPECT_TRUE(is_success(StatusCode::PartialContent));
  EXPECT_FALSE(is_success(StatusCode::MovedPermanently));
  EXPECT_FALSE(is_success(StatusCode::BadRequest));
}

TEST(StatusClassification, Redirect) {
  EXPECT_TRUE(is_redirect(StatusCode::MovedPermanently));
  EXPECT_TRUE(is_redirect(StatusCode::Found));
  EXPECT_TRUE(is_redirect(StatusCode::NotModified));
  EXPECT_TRUE(is_redirect(StatusCode::PermanentRedirect));
  EXPECT_FALSE(is_redirect(StatusCode::Ok));
  EXPECT_FALSE(is_redirect(StatusCode::NotFound));
}

TEST(StatusClassification, ClientError) {
  EXPECT_TRUE(is_client_error(StatusCode::BadRequest));
  EXPECT_TRUE(is_client_error(StatusCode::Unauthorized));
  EXPECT_TRUE(is_client_error(StatusCode::NotFound));
  EXPECT_TRUE(is_client_error(StatusCode::TooManyRequests));
  EXPECT_FALSE(is_client_error(StatusCode::Ok));
  EXPECT_FALSE(is_client_error(StatusCode::InternalServerError));
}

TEST(StatusClassification, ServerError) {
  EXPECT_TRUE(is_server_error(StatusCode::InternalServerError));
  EXPECT_TRUE(is_server_error(StatusCode::BadGateway));
  EXPECT_TRUE(is_server_error(StatusCode::GatewayTimeout));
  EXPECT_FALSE(is_server_error(StatusCode::NotFound));
  EXPECT_FALSE(is_server_error(StatusCode::Ok));
}

TEST(StatusClassification, UnknownCodesByRange) {
  // Codes outside the enum but within the range still classify by range.
  EXPECT_TRUE(is_success(static_cast<StatusCode>(299)));
  EXPECT_TRUE(is_redirect(static_cast<StatusCode>(399)));
  EXPECT_TRUE(is_client_error(static_cast<StatusCode>(499)));
  EXPECT_TRUE(is_server_error(static_cast<StatusCode>(599)));
  // Out-of-range codes are false for every predicate.
  EXPECT_FALSE(is_informational(static_cast<StatusCode>(99)));
  EXPECT_FALSE(is_informational(static_cast<StatusCode>(600)));
}

/* ───────────────────────────────────────────────────────────────────
 *  to_string — canonical reason phrases
 * ─────────────────────────────────────────────────────────────────── */

TEST(StatusToString, CommonCodes) {
  EXPECT_STREQ(to_string(StatusCode::Ok), "OK");
  EXPECT_STREQ(to_string(StatusCode::Created), "Created");
  EXPECT_STREQ(to_string(StatusCode::NoContent), "No Content");
  EXPECT_STREQ(to_string(StatusCode::MovedPermanently), "Moved Permanently");
  EXPECT_STREQ(to_string(StatusCode::NotFound), "Not Found");
  EXPECT_STREQ(to_string(StatusCode::MethodNotAllowed), "Method Not Allowed");
  EXPECT_STREQ(to_string(StatusCode::InternalServerError), "Internal Server Error");
  EXPECT_STREQ(to_string(StatusCode::ServiceUnavailable), "Service Unavailable");
}

TEST(StatusToString, UnknownCodeReturnsEmpty) {
  EXPECT_STREQ(to_string(static_cast<StatusCode>(299)), "");
  EXPECT_STREQ(to_string(static_cast<StatusCode>(599)), "");
}

/* ───────────────────────────────────────────────────────────────────
 *  from_string — parsing
 * ─────────────────────────────────────────────────────────────────── */

TEST(StatusFromString, DigitsOnly) {
  auto r = from_string(String::from_utf8("200").unwrap());
  ASSERT_TRUE(r.is_some());
  EXPECT_EQ(r.unwrap(), StatusCode::Ok);

  r = from_string(String::from_utf8("404").unwrap());
  ASSERT_TRUE(r.is_some());
  EXPECT_EQ(r.unwrap(), StatusCode::NotFound);
}

TEST(StatusFromString, WithReasonPhrase) {
  // "404 Not Found" should parse as 404.
  auto r = from_string(String::from_utf8("404 Not Found").unwrap());
  ASSERT_TRUE(r.is_some());
  EXPECT_EQ(r.unwrap(), StatusCode::NotFound);
}

TEST(StatusFromString, WithLeadingWhitespace) {
  auto r = from_string(String::from_utf8("  200  ").unwrap());
  ASSERT_TRUE(r.is_some());
  EXPECT_EQ(r.unwrap(), StatusCode::Ok);
}

TEST(StatusFromString, UnknownCodeInRange) {
  // 299 is not in the enum but is within 100–599.
  auto r = from_string(String::from_utf8("299").unwrap());
  ASSERT_TRUE(r.is_some());
  EXPECT_EQ(static_cast<uint16_t>(r.unwrap()), 299u);
}

TEST(StatusFromString, OutOfRange) {
  EXPECT_TRUE(from_string(String::from_utf8("99").unwrap()).is_none());
  EXPECT_TRUE(from_string(String::from_utf8("600").unwrap()).is_none());
  EXPECT_TRUE(from_string(String::from_utf8("abc").unwrap()).is_none());
  EXPECT_TRUE(from_string(String::from_utf8("").unwrap()).is_none());
}
