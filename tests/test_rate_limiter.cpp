#include <gtest/gtest.h>

#include "rate_limiter.h"

using namespace std::chrono_literals;

struct FakeClock {
  RateLimiter::Clock::time_point t = RateLimiter::Clock::now();
  std::function<RateLimiter::Clock::time_point()> fn() { return [this] { return t; }; }
};

TEST(RateLimiter, BurstThenDeny) {
  FakeClock clk;
  RateLimiter rl(10, 5, clk.fn());
  for (int i = 0; i < 5; ++i) EXPECT_TRUE(rl.allow("1.1.1.1")) << i;
  EXPECT_FALSE(rl.allow("1.1.1.1"));
  EXPECT_FALSE(rl.allow("1.1.1.1"));
  auto s = rl.stats();
  EXPECT_EQ(s.allowed, 5u);
  EXPECT_EQ(s.denied, 2u);
  EXPECT_EQ(s.clients, 1u);
}

TEST(RateLimiter, RefillsAtRate) {
  FakeClock clk;
  RateLimiter rl(10, 5, clk.fn());
  for (int i = 0; i < 5; ++i) rl.allow("a");
  EXPECT_FALSE(rl.allow("a"));
  clk.t += 250ms;  // 2.5 tokens
  EXPECT_TRUE(rl.allow("a"));
  EXPECT_TRUE(rl.allow("a"));
  EXPECT_FALSE(rl.allow("a"));  // 0.5 left
  clk.t += 10s;    // caps at burst, not 100 tokens
  for (int i = 0; i < 5; ++i) EXPECT_TRUE(rl.allow("a")) << i;
  EXPECT_FALSE(rl.allow("a"));
}

TEST(RateLimiter, ClientsAreIsolated) {
  FakeClock clk;
  RateLimiter rl(1, 2, clk.fn());
  EXPECT_TRUE(rl.allow("a"));
  EXPECT_TRUE(rl.allow("a"));
  EXPECT_FALSE(rl.allow("a"));
  EXPECT_TRUE(rl.allow("b"));
  EXPECT_TRUE(rl.allow("b"));
  EXPECT_FALSE(rl.allow("b"));
  EXPECT_EQ(rl.stats().clients, 2u);
}

TEST(RateLimiter, IdleBucketsAreSwept) {
  FakeClock clk;
  RateLimiter rl(1, 1, clk.fn());
  rl.allow("a");
  clk.t += 11min;
  rl.allow("b");  // sweep runs here, "a" idle > 10 min is dropped
  EXPECT_EQ(rl.stats().clients, 1u);
  EXPECT_TRUE(rl.allow("a"));  // fresh bucket, full burst
}
