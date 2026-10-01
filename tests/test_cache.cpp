#include <gtest/gtest.h>

#include "cache.h"

using namespace std::chrono_literals;

static std::shared_ptr<const CacheEntry> entry(std::string body, LruCache::Clock::time_point created = LruCache::Clock::now()) {
  return std::make_shared<const CacheEntry>(CacheEntry{std::move(body), "text/plain", created});
}

TEST(Cache, HitMissCounting) {
  LruCache c(1024, 0s);
  EXPECT_EQ(c.get("a"), nullptr);
  c.put("a", entry("hello"));
  auto e = c.get("a");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->body, "hello");
  EXPECT_EQ(e->mime, "text/plain");
  auto s = c.stats();
  EXPECT_EQ(s.hits, 1u);
  EXPECT_EQ(s.misses, 1u);
  EXPECT_EQ(s.entries, 1u);
  EXPECT_EQ(s.bytes, 5u);
}

TEST(Cache, EvictsLeastRecentlyUsed) {
  LruCache c(250, 0s);  // room for two 100-byte entries
  c.put("a", entry(std::string(100, 'a')));
  c.put("b", entry(std::string(100, 'b')));
  c.put("c", entry(std::string(100, 'c')));  // evicts a
  EXPECT_EQ(c.get("a"), nullptr);
  EXPECT_NE(c.get("b"), nullptr);               // b is now most recent
  c.put("d", entry(std::string(100, 'd')));  // evicts c, not b
  EXPECT_EQ(c.get("c"), nullptr);
  EXPECT_NE(c.get("b"), nullptr);
  EXPECT_NE(c.get("d"), nullptr);
  EXPECT_EQ(c.stats().entries, 2u);
  EXPECT_LE(c.stats().bytes, 250u);
}

TEST(Cache, ReplaceAdjustsBytes) {
  LruCache c(1024, 0s);
  c.put("a", entry(std::string(100, 'x')));
  c.put("a", entry(std::string(30, 'y')));
  EXPECT_EQ(c.stats().bytes, 30u);
  EXPECT_EQ(c.stats().entries, 1u);
  EXPECT_EQ(c.get("a")->body.size(), 30u);
}

TEST(Cache, OversizedEntrySkipped) {
  LruCache c(50, 0s);
  c.put("big", entry(std::string(51, 'x')));
  EXPECT_EQ(c.get("big"), nullptr);
  EXPECT_EQ(c.stats().entries, 0u);
  c.put("fits", entry(std::string(50, 'x')));
  EXPECT_NE(c.get("fits"), nullptr);
}

TEST(Cache, TtlExpiry) {
  auto t0 = LruCache::Clock::now();
  auto fake_now = t0;
  LruCache c(1024, 10s, [&] { return fake_now; });
  c.put("a", entry("v", t0));
  fake_now = t0 + 9s;
  EXPECT_NE(c.get("a"), nullptr);
  fake_now = t0 + 10s;
  EXPECT_EQ(c.get("a"), nullptr);  // expired exactly at ttl
  EXPECT_EQ(c.stats().entries, 0u);
  EXPECT_EQ(c.stats().bytes, 0u);
}

TEST(Cache, ZeroTtlNeverExpires) {
  auto t0 = LruCache::Clock::now();
  LruCache c(1024, 0s, [&] { return t0 + 24h * 365; });
  c.put("a", entry("v", t0));
  EXPECT_NE(c.get("a"), nullptr);
}
