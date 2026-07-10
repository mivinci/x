/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 */

#include <gtest/gtest.h>

#include <map>
#include <string>

#include <xpp/http/header.h>
#include <xpp/option.h>

using xpp::http::HeaderMap;
using xpp::none;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Construction & simple access
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(HeaderMapTest, DefaultConstructionEmpty) {
  HeaderMap h;
  EXPECT_TRUE(h.empty());
  EXPECT_EQ(h.size(), 0u);
  EXPECT_EQ(h.begin(), h.end());
}

TEST(HeaderMapTest, InsertAndGet) {
  HeaderMap h;
  h.insert("Content-Type", "text/html");

  auto v = h.get("content-type");
  ASSERT_TRUE(v.is_some());
  EXPECT_EQ(v.unwrap(), "text/html");
}

TEST(HeaderMapTest, SizeAfterInsert) {
  HeaderMap h;
  EXPECT_EQ(h.size(), 0u);

  h.insert("a", "1");
  EXPECT_EQ(h.size(), 1u);

  h.insert("b", "2");
  EXPECT_EQ(h.size(), 2u);
}

TEST(HeaderMapTest, GetNonExistentKey) {
  HeaderMap h;
  auto v = h.get("host");
  EXPECT_TRUE(v.is_none());
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Case insensitivity
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(HeaderMapTest, CaseInsensitiveExactMatch) {
  HeaderMap h;
  h.insert("Content-Type", "text/html");
  EXPECT_EQ(h.get("Content-Type").unwrap(), "text/html");
}

TEST(HeaderMapTest, CaseInsensitiveLowercaseLookup) {
  HeaderMap h;
  h.insert("Content-Type", "text/html");
  EXPECT_EQ(h.get("content-type").unwrap(), "text/html");
}

TEST(HeaderMapTest, CaseInsensitiveUppercaseLookup) {
  HeaderMap h;
  h.insert("Content-Type", "text/html");
  EXPECT_EQ(h.get("CONTENT-TYPE").unwrap(), "text/html");
}

TEST(HeaderMapTest, CaseInsensitiveMixedLookup) {
  HeaderMap h;
  h.insert("Content-Type", "text/html");
  EXPECT_EQ(h.get("CoNtEnT-tYpE").unwrap(), "text/html");
}

TEST(HeaderMapTest, CaseInsensitiveAllVariants) {
  HeaderMap h;
  h.insert("X-Custom-Header", "value");
  EXPECT_EQ(h.get("X-Custom-Header").unwrap(), "value");
  EXPECT_EQ(h.get("x-custom-header").unwrap(), "value");
  EXPECT_EQ(h.get("X-CUSTOM-HEADER").unwrap(), "value");
  EXPECT_EQ(h.get("x-Custom-header").unwrap(), "value");
}

TEST(HeaderMapTest, CaseInsensitiveDifferentInputCases) {
  HeaderMap h;
  h.insert("Content-TYPE", "A");
  h.insert("CONTENT-type", "B");
  h.insert("content-Type", "C");

  // All 3 map to the same lowercased key "content-type"
  auto vals = h.get_all("content-type");
  int  count = 0;
  for (auto &v : vals) ++count;
  EXPECT_EQ(count, 3);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Multi-valued headers (get_all)
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(HeaderMapTest, GetAllMultipleValues) {
  HeaderMap h;
  h.insert("set-cookie", "a=1");
  h.insert("set-cookie", "b=2");
  h.insert("set-cookie", "c=3");

  int           count = 0;
  std::string   vals;
  for (auto &v : h.get_all("set-cookie")) {
    if (count > 0) vals += ",";
    vals += v;
    ++count;
  }
  EXPECT_EQ(count, 3);
  EXPECT_EQ(vals, "a=1,b=2,c=3");
}

TEST(HeaderMapTest, GetAllSingleValue) {
  HeaderMap h;
  h.insert("host", "localhost");

  int count = 0;
  for (auto &v : h.get_all("host")) {
    EXPECT_EQ(v, "localhost");
    ++count;
  }
  EXPECT_EQ(count, 1);
}

TEST(HeaderMapTest, GetAllEmptyForMissingKey) {
  HeaderMap h;
  int       count = 0;
  for (auto &v : h.get_all("nope")) {
    (void)v;
    ++count;
  }
  EXPECT_EQ(count, 0);
}

TEST(HeaderMapTest, GetAllEmptyRangeCheck) {
  HeaderMap h;
  auto      vals = h.get_all("missing");
  EXPECT_TRUE(vals.empty());
}

TEST(HeaderMapTest, GetAllCaseInsensitive) {
  HeaderMap h;
  h.insert("X-Foo", "one");
  h.insert("x-foo", "two");

  int count = 0;
  for (auto &v : h.get_all("X-FOO")) ++count;
  EXPECT_EQ(count, 2);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Get returns first value
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(HeaderMapTest, GetReturnsFirstValue) {
  HeaderMap h;
  h.insert("x", "first");
  h.insert("x", "second");
  EXPECT_EQ(h.get("x").unwrap(), "first");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Contains
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(HeaderMapTest, ContainsExistingKey) {
  HeaderMap h;
  h.insert("host", "example.com");
  EXPECT_TRUE(h.contains("host"));
}

TEST(HeaderMapTest, ContainsNonExistingKey) {
  HeaderMap h;
  EXPECT_FALSE(h.contains("host"));
}

TEST(HeaderMapTest, ContainsCaseInsensitive) {
  HeaderMap h;
  h.insert("Content-Type", "text/html");
  EXPECT_TRUE(h.contains("content-type"));
  EXPECT_TRUE(h.contains("CONTENT-TYPE"));
  EXPECT_TRUE(h.contains("Content-Type"));
  EXPECT_FALSE(h.contains("content-length"));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Erase
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(HeaderMapTest, EraseExistingKey) {
  HeaderMap h;
  h.insert("x-foo", "bar");
  EXPECT_EQ(h.size(), 1u);

  h.erase("x-foo");
  EXPECT_EQ(h.size(), 0u);
  EXPECT_TRUE(h.get("x-foo").is_none());
}

TEST(HeaderMapTest, EraseCaseInsensitive) {
  HeaderMap h;
  h.insert("X-Foo", "bar");
  h.erase("x-foo");
  EXPECT_TRUE(h.get("X-Foo").is_none());
}

TEST(HeaderMapTest, EraseNonExistentKey) {
  HeaderMap h;
  h.insert("host", "x");
  EXPECT_EQ(h.size(), 1u);
  h.erase("nope"); // no-op, not a crash
  EXPECT_EQ(h.size(), 1u);
}

TEST(HeaderMapTest, EraseOnlyAffectsMatchingKey) {
  HeaderMap h;
  h.insert("a", "1");
  h.insert("b", "2");
  h.erase("a");
  EXPECT_EQ(h.size(), 1u);
  EXPECT_TRUE(h.get("a").is_none());
  EXPECT_TRUE(h.get("b").is_some());
}

TEST(HeaderMapTest, EraseAllDuplicateKeys) {
  HeaderMap h;
  h.insert("x", "1");
  h.insert("x", "2");
  h.insert("x", "3");
  EXPECT_EQ(h.size(), 3u);

  h.erase("x");
  EXPECT_EQ(h.size(), 0u);

  int count = 0;
  for (auto &v : h.get_all("x")) { (void)v; ++count; }
  EXPECT_EQ(count, 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Zero-copy — reference validity
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(HeaderMapTest, GetReferencePointsToMapStorage) {
  HeaderMap h;
  h.insert("x", "original");

  auto v = h.get("x");
  ASSERT_TRUE(v.is_some());

  // v is a reference into h's multimap — should read "original"
  EXPECT_EQ(v.unwrap(), "original");
}

TEST(HeaderMapTest, GetAllValuesAreReferences) {
  HeaderMap h;
  h.insert("x", "hello");

  for (auto &v : h.get_all("x")) {
    // Modify through the reference should NOT work — const ref
    // Compilation check: v is const std::string&
    static_assert(std::is_const<std::remove_reference<decltype(v)>::type>::value,
                  "get_all values must be const");
    EXPECT_EQ(v, "hello");
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Iteration (global begin/end)
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(HeaderMapTest, IterateEmptyMap) {
  HeaderMap h;
  int       count = 0;
  for (auto &kv : h) {
    (void)kv;
    ++count;
  }
  EXPECT_EQ(count, 0);
}

TEST(HeaderMapTest, IterateNonEmptyMap) {
  HeaderMap h;
  h.insert("a", "1");
  h.insert("b", "2");
  h.insert("c", "3");

  int            count    = 0;
  std::set<std::string> seen;
  for (auto &kv : h) {
    seen.insert(kv.first);
    ++count;
  }
  EXPECT_EQ(count, 3);
  EXPECT_TRUE(seen.count("a"));
  EXPECT_TRUE(seen.count("b"));
  EXPECT_TRUE(seen.count("c"));
}

TEST(HeaderMapTest, IterateSeesLowercaseKeys) {
  HeaderMap h;
  h.insert("Content-Type", "text/html");
  h.insert("X-Custom", "val");

  for (auto &kv : h) {
    // Keys must be all-lowercase
    for (char c : kv.first) {
      EXPECT_FALSE(c >= 'A' && c <= 'Z') << "Key " << kv.first << " has uppercase";
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  raw() access
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(HeaderMapTest, RawAccessReturnsUnderlyingMap) {
  HeaderMap h;
  h.insert("content-type", "text/html");

  auto &raw = h.raw();
  auto  it  = raw.find("content-type");
  ASSERT_NE(it, raw.end());
  EXPECT_EQ(it->second, "text/html");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Edge cases
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(HeaderMapTest, EmptyStringValue) {
  HeaderMap h;
  h.insert("x-empty", "");

  auto v = h.get("x-empty");
  ASSERT_TRUE(v.is_some());
  EXPECT_TRUE((v.unwrap()).empty());
  EXPECT_EQ(v.unwrap(), "");
}

TEST(HeaderMapTest, EmptyStringKeyIsValid) {
  HeaderMap h;
  h.insert("", "empty-key"); // weird but shouldn't crash

  auto v = h.get("");
  ASSERT_TRUE(v.is_some());
  EXPECT_EQ(v.unwrap(), "empty-key");
}

TEST(HeaderMapTest, SpecialCharactersInValue) {
  HeaderMap h;
  h.insert("x-special", "value; param=1, param2=3, \"quoted\"");

  auto v = h.get("x-special");
  ASSERT_TRUE(v.is_some());
  EXPECT_EQ(v.unwrap(), "value; param=1, param2=3, \"quoted\"");
}

TEST(HeaderMapTest, NumberOnlyHeaderName) {
  HeaderMap h;
  h.insert("123", "numeric");

  auto v = h.get("123");
  ASSERT_TRUE(v.is_some());
  EXPECT_EQ(v.unwrap(), "numeric");
}

TEST(HeaderMapTest, HyphenOnlyHeaderName) {
  HeaderMap h;
  h.insert("---", "dashes");

  auto v = h.get("---");
  ASSERT_TRUE(v.is_some());
  EXPECT_EQ(v.unwrap(), "dashes");
}

TEST(HeaderMapTest, LargeNumberOfHeaders) {
  HeaderMap h;
  for (int i = 0; i < 1000; ++i) {
    h.insert("x-" + std::to_string(i), std::to_string(i * 2));
  }
  EXPECT_EQ(h.size(), 1000u);

  // Spot-check a few
  EXPECT_EQ(h.get("x-0").unwrap(), "0");
  EXPECT_EQ(h.get("x-500").unwrap(), "1000");
  EXPECT_EQ(h.get("x-999").unwrap(), "1998");
}

TEST(HeaderMapTest, VeryLongValue) {
  HeaderMap h;
  std::string long_val(10000, 'X');
  h.insert("x-long", long_val);

  auto v = h.get("x-long");
  ASSERT_TRUE(v.is_some());
  EXPECT_EQ((v.unwrap()).size(), 10000u);
  EXPECT_EQ(v.unwrap(), long_val);
}

TEST(HeaderMapTest, ContentLengthAndTransferEncoding) {
  HeaderMap h;
  h.insert("content-length", "1024");
  h.insert("transfer-encoding", "chunked");

  EXPECT_EQ(h.get("content-length").unwrap(), "1024");
  EXPECT_EQ(h.get("transfer-encoding").unwrap(), "chunked");
}

TEST(HeaderMapTest, CopyConstruction) {
  HeaderMap h;
  h.insert("x", "y");

  HeaderMap h2 = h;
  EXPECT_EQ(h2.get("x").unwrap(), "y");
  EXPECT_EQ(h2.size(), 1u);

  // Independent — modifying copy doesn't affect original
  h2.insert("z", "w");
  EXPECT_EQ(h.size(), 1u);
  EXPECT_EQ(h2.size(), 2u);
}

TEST(HeaderMapTest, MoveConstruction) {
  HeaderMap h;
  h.insert("x", "y");

  HeaderMap h2 = std::move(h);
  EXPECT_EQ(h2.get("x").unwrap(), "y");
  EXPECT_EQ(h2.size(), 1u);
  // moved-from is in valid-but-unspecified state
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Const correctness
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(HeaderMapTest, ConstHeaderMapQuery) {
  const HeaderMap h = [] {
    HeaderMap tmp;
    tmp.insert("x", "const-test");
    return tmp;
  }();

  auto v = h.get("x");
  ASSERT_TRUE(v.is_some());
  EXPECT_EQ(v.unwrap(), "const-test");

  // get_all on const
  int count = 0;
  for (auto &val : h.get_all("x")) {
    EXPECT_EQ(val, "const-test");
    ++count;
  }
  EXPECT_EQ(count, 1);

  // begin/end on const
  count = 0;
  for (const auto &kv : h) ++count;
  EXPECT_EQ(count, 1);
}
