/*
 * Unit tests for xpp::http::HeaderMap.
 */

#include <gtest/gtest.h>
#include <xpp/http/header.h>

using namespace xpp;
using namespace xpp::http;

/* ───────────────────────────────────────────────────────────────────
 *  Construction & empty state
 * ─────────────────────────────────────────────────────────────────── */

TEST(HeaderMapConstruction, DefaultEmpty) {
  HeaderMap h;
  EXPECT_TRUE(h.empty());
  EXPECT_EQ(h.size(), 0u);
  EXPECT_TRUE(h.begin() == h.end());
}

TEST(HeaderMapConstruction, FromVec) {
  Vec<std::pair<String, String>> entries;
  entries.push(std::make_pair(String::from_utf8("Content-Type").unwrap(),
                              String::from_utf8("text/plain").unwrap()));
  entries.push(
    std::make_pair(String::from_utf8("X-Custom").unwrap(), String::from_utf8("v1").unwrap()));

  HeaderMap h = HeaderMap::from_vec(std::move(entries));
  EXPECT_EQ(h.size(), 2u);
  EXPECT_TRUE(h.contains("content-type"));
  EXPECT_TRUE(h.contains("CONTENT-TYPE"));
  EXPECT_EQ(h.get("content-type").unwrap(), "text/plain");
}

/* ───────────────────────────────────────────────────────────────────
 *  Insert & get (case-insensitive)
 * ─────────────────────────────────────────────────────────────────── */

TEST(HeaderMapInsert, CaseInsensitive) {
  HeaderMap h;
  h.insert("Content-Type", "text/plain");
  h.insert("X-Custom", "v1");

  EXPECT_EQ(h.size(), 2u);
  EXPECT_EQ(h.get("content-type").unwrap(), "text/plain");
  EXPECT_EQ(h.get("CONTENT-TYPE").unwrap(), "text/plain");
  EXPECT_EQ(h.get("Content-Type").unwrap(), "text/plain");
  EXPECT_EQ(h.get("content-type").unwrap(), "text/plain");
}

TEST(HeaderMapInsert, PreservesValueCase) {
  HeaderMap h;
  h.insert("Content-Type", "Text/PLAIN");
  EXPECT_EQ(h.get("content-type").unwrap(), "Text/PLAIN");
}

TEST(HeaderMapInsert, DuplicateKey) {
  HeaderMap h;
  h.insert("Set-Cookie", "a=1");
  h.insert("Set-Cookie", "b=2");

  // get returns the first matching value.
  EXPECT_EQ(h.get("set-cookie").unwrap(), "a=1");
  EXPECT_EQ(h.size(), 2u);
}

TEST(HeaderMapInsert, CStringOverload) {
  HeaderMap h;
  h.insert("Content-Type", "application/json");
  EXPECT_EQ(h.get("content-type").unwrap(), "application/json");
}

/* ───────────────────────────────────────────────────────────────────
 *  contains & get (missing key)
 * ─────────────────────────────────────────────────────────────────── */

TEST(HeaderMapGet, MissingKey) {
  HeaderMap h;
  h.insert("Content-Type", "text/plain");

  EXPECT_FALSE(h.contains("Authorization"));
  EXPECT_TRUE(h.get("authorization").is_none());
}

TEST(HeaderMapGet, EmptyStringKey) {
  HeaderMap h;
  h.insert("", "empty-key-value");
  EXPECT_TRUE(h.contains(""));
  EXPECT_EQ(h.get("").unwrap(), "empty-key-value");
}

/* ───────────────────────────────────────────────────────────────────
 *  Multi-value (Set-Cookie)
 * ─────────────────────────────────────────────────────────────────── */

TEST(HeaderMapGetAll, MultipleValues) {
  HeaderMap h;
  h.insert("Set-Cookie", "a=1");
  h.insert("Set-Cookie", "b=2");
  h.insert("Set-Cookie", "c=3");

  auto values = h.get_all("set-cookie");
  EXPECT_EQ(values.size(), 3u);

  auto it = values.begin();
  EXPECT_EQ(*it, String::from_utf8("a=1").unwrap());
  ++it;
  EXPECT_EQ(*it, String::from_utf8("b=2").unwrap());
  ++it;
  EXPECT_EQ(*it, String::from_utf8("c=3").unwrap());
  ++it;
  EXPECT_TRUE(it == values.end());
}

TEST(HeaderMapGetAll, NoMatch) {
  HeaderMap h;
  h.insert("Content-Type", "text/plain");

  auto values = h.get_all("set-cookie");
  EXPECT_TRUE(values.empty());
  EXPECT_EQ(values.size(), 0u);
  EXPECT_TRUE(values.begin() == values.end());
}

TEST(HeaderMapGetAll, CaseInsensitive) {
  HeaderMap h;
  h.insert("Set-Cookie", "a=1");

  auto values = h.get_all("SET-COOKIE");
  EXPECT_EQ(values.size(), 1u);
  EXPECT_EQ(*values.begin(), String::from_utf8("a=1").unwrap());
}

/* ───────────────────────────────────────────────────────────────────
 *  erase
 * ─────────────────────────────────────────────────────────────────── */

TEST(HeaderMapErase, SingleMatch) {
  HeaderMap h;
  h.insert("Content-Type", "text/plain");
  h.insert("Accept", "application/json");

  size_t erased = h.erase("content-type");
  EXPECT_EQ(erased, 1u);
  EXPECT_FALSE(h.contains("Content-Type"));
  EXPECT_TRUE(h.contains("Accept"));
  EXPECT_EQ(h.size(), 1u);
}

TEST(HeaderMapErase, MultipleMatches) {
  HeaderMap h;
  h.insert("Set-Cookie", "a=1");
  h.insert("Set-Cookie", "b=2");
  h.insert("Set-Cookie", "c=3");

  size_t erased = h.erase("set-cookie");
  EXPECT_EQ(erased, 3u);
  EXPECT_FALSE(h.contains("Set-Cookie"));
  EXPECT_EQ(h.size(), 0u);
}

TEST(HeaderMapErase, NoMatch) {
  HeaderMap h;
  h.insert("Content-Type", "text/plain");
  size_t erased = h.erase("authorization");
  EXPECT_EQ(erased, 0u);
  EXPECT_EQ(h.size(), 1u);
}

TEST(HeaderMapErase, CaseInsensitive) {
  HeaderMap h;
  h.insert("Content-Type", "text/plain");
  h.erase("CONTENT-TYPE");
  EXPECT_EQ(h.size(), 0u);
}

/* ───────────────────────────────────────────────────────────────────
 *  Iteration
 * ─────────────────────────────────────────────────────────────────── */

TEST(HeaderMapIteration, ForRange) {
  HeaderMap h;
  h.insert("Content-Type", "text/plain");
  h.insert("Accept", "application/json");
  h.insert("X-Custom", "v1");

  size_t count = 0;
  for (const auto &entry : h) {
    (void)entry;
    ++count;
  }
  EXPECT_EQ(count, 3u);
}

TEST(HeaderMapIteration, KeysAndValuesParallel) {
  HeaderMap h;
  h.insert("Content-Type", "text/plain");
  h.insert("Set-Cookie", "a=1");

  // Direct array access — parallel arrays stay in sync.
  EXPECT_EQ(h.keys().len(), h.values().len());
  EXPECT_EQ(h.keys()[0], String::from_utf8("content-type").unwrap());
  EXPECT_EQ(h.values()[0], String::from_utf8("text/plain").unwrap());
}

/* ───────────────────────────────────────────────────────────────────
 *  clear
 * ─────────────────────────────────────────────────────────────────── */

TEST(HeaderMapClear, Empties) {
  HeaderMap h;
  h.insert("Content-Type", "text/plain");
  h.insert("Accept", "application/json");

  h.clear();
  EXPECT_TRUE(h.empty());
  EXPECT_EQ(h.size(), 0u);
}

/* ───────────────────────────────────────────────────────────────────
 *  Move semantics
 * ─────────────────────────────────────────────────────────────────── */

TEST(HeaderMapMove, MoveConstructor) {
  HeaderMap h1;
  h1.insert("Content-Type", "text/plain");

  HeaderMap h2(std::move(h1));
  EXPECT_EQ(h2.size(), 1u);
  EXPECT_EQ(h2.get("content-type").unwrap(), "text/plain");
}
